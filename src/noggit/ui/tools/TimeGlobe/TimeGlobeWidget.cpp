// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/ui/tools/TimeGlobe/TimeGlobeWidget.hpp>
#include <noggit/MapView.h>
#include <noggit/World.h>

#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtWidgets/QSlider>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QFrame>

#include <algorithm>
#include <cmath>

namespace
{
  constexpr int FRAME_W = 256, FRAME_H = 128;
  constexpr int DISPLAY_W = 240, DISPLAY_H = 120; // native 256x128 aspect
  constexpr float PI = 3.14159265358979323846f;

  // A glow texture is white with alpha = brightness. Tint = solid colour masked by that alpha, so only the
  // bright parts carry colour and the transparent background stays transparent (no rectangle in any blend).
  QImage tintGlow(QImage const& glow, QColor const& color)
  {
    QImage g = glow.convertToFormat(QImage::Format_ARGB32);
    QImage out(g.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.fillRect(out.rect(), color);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, g);
    p.end();
    return out;
  }

  inline void sampleStrip(QImage const& strip, float u, float v, float& r, float& g, float& b)
  {
    int const sw = strip.width(), sh = strip.height();
    u = u - std::floor(u);
    v = std::clamp(v, 0.f, 1.f);
    float const fu = u * sw - 0.5f, fv = v * (sh - 1);
    int const x0 = static_cast<int>(std::floor(fu)), y0 = static_cast<int>(std::floor(fv));
    float const tx = fu - x0, ty = fv - y0;
    int const x0m = ((x0 % sw) + sw) % sw, x1m = (x0m + 1) % sw;
    int const y0c = std::clamp(y0, 0, sh - 1), y1c = std::clamp(y0 + 1, 0, sh - 1);
    QRgb const c00 = strip.pixel(x0m, y0c), c10 = strip.pixel(x1m, y0c);
    QRgb const c01 = strip.pixel(x0m, y1c), c11 = strip.pixel(x1m, y1c);
    auto lerp = [](float a, float c, float t) { return a + (c - a) * t; };
    r = lerp(lerp(qRed(c00),   qRed(c10),   tx), lerp(qRed(c01),   qRed(c11),   tx), ty);
    g = lerp(lerp(qGreen(c00), qGreen(c10), tx), lerp(qGreen(c01), qGreen(c11), tx), ty);
    b = lerp(lerp(qBlue(c00),  qBlue(c10),  tx), lerp(qBlue(c01),  qBlue(c11),  tx), ty);
  }
}

namespace Noggit::Ui
{
  TimeGlobeWidget::TimeGlobeWidget(MapView* mapView, QWidget* parent)
    : QWidget(parent), _mapView(mapView)
  {
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true); // so mouseMoveEvent can show the hand cursor only over the orb
    setToolTip("Time of day");

    _strip = QImage(":/timeglobe/globe").convertToFormat(QImage::Format_ARGB32);
    QImage const glowStud = QImage(":/timeglobe/glow-stud").convertToFormat(QImage::Format_ARGB32);
    QImage const glowHalo = QImage(":/timeglobe/glow-halo").convertToFormat(QImage::Format_ARGB32);
    _glowStudDay   = tintGlow(glowStud, _dayCol);
    _glowStudNight = tintGlow(glowStud, _nightCol);
    _glowHaloDay   = tintGlow(glowHalo, _dayCol);
    _glowHaloNight = tintGlow(glowHalo, _nightCol);

    auto mkRace = [](QString key, char const* res, float cx, float cy, float r, float studR)
    {
      RaceDef d;
      d.key = std::move(key);
      d.frame = QImage(res).convertToFormat(QImage::Format_ARGB32);
      d.cx = cx; d.cy = cy; d.r = r; d.studR = studR;
      return d;
    };
    _races.push_back(mkRace("human",    ":/timeglobe/frame-human",    125.4f, 44.4f, 36.f, 35.0f));
    _races.push_back(mkRace("orc",      ":/timeglobe/frame-orc",      126.3f, 43.7f, 37.f, 35.0f));
    _races.push_back(mkRace("nightelf", ":/timeglobe/frame-nightelf", 125.2f, 43.0f, 37.f, 32.5f));
    _races.push_back(mkRace("undead",   ":/timeglobe/frame-undead",   125.5f, 43.7f, 35.f, 34.0f));

    _globeImg = QImage(FRAME_W, FRAME_H, QImage::Format_ARGB32);

    QSettings settings;
    setRace(settings.value("theme/race", "nightelf").toString());

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, [this, poll = 0]() mutable
    {
      // ~once a second, pick up a live "Time globe skin" change from settings
      if (++poll >= 15)
      {
        poll = 0;
        QSettings s;
        setRace(s.value("theme/race", "nightelf").toString());
      }
      if (std::abs(currentMinutes() - _lastMinutes) > 0.05f)
        update();
    });
    _timer->start(66);
  }

  QSize TimeGlobeWidget::sizeHint() const
  {
    return QSize(DISPLAY_W, DISPLAY_H);
  }

  void TimeGlobeWidget::setRace(QString const& raceKey)
  {
    for (int i = 0; i < static_cast<int>(_races.size()); ++i)
    {
      if (_races[i].key == raceKey)
      {
        if (_raceIdx != i) { _raceIdx = i; _lutRace = -1; update(); }
        return;
      }
    }
  }

  float TimeGlobeWidget::currentMinutes() const
  {
    World* w = _mapView ? _mapView->getWorld() : nullptr;
    if (!w) return 720.f;
    float t = std::fmod(w->time, 2880.f);
    if (t < 0.f) t += 2880.f;
    return t / 2.f; // World::time is 0..2880 half-minutes -> 0..1440 minutes
  }

  void TimeGlobeWidget::studPositions(RaceDef const& race, std::array<QPointF, 8>& out) const
  {
    // per-race hand nudges (native px), on top of the general E +2 / W -2 spread. k -> angle 45*k:
    // 0=E, 1=NE, 2=N, 3=NW, 4=W, 5=SW, 6=S, 7=SE.
    auto adjust = [&race](int k, float& dx, float& dy)
    {
      dx = 0.f; dy = 0.f;
      if (race.key == "nightelf")
      {
        switch (k)
        {
          case 0: dx = 3; dy = 3;  break;
          case 1: dx = 2; dy = -1; break;
          case 4: dx = -1; dy = 2; break;
          case 5: dx = -1; dy = 2; break;
          case 6: dx = 1; dy = 2;  break;
          case 7: dx = 3; dy = 3;  break;
          default: break;
        }
      }
      else if (race.key == "undead")
      {
        if (k == 0 || k == 1 || k == 7) dx = 1; // east balls +1
      }
    };

    for (int k = 0; k < 8; ++k)
    {
      float const a = k * 45.f * PI / 180.f;
      float const c = std::cos(a);
      float x = race.cx + race.studR * c;
      float y = race.cy - race.studR * std::sin(a);
      if (c > 0.3f) x += 2.f; else if (c < -0.3f) x -= 2.f;
      float dx, dy; adjust(k, dx, dy);
      out[k] = QPointF(x + dx, y + dy);
    }
  }

  void TimeGlobeWidget::buildLut()
  {
    RaceDef const& R = _races[_raceIdx];
    _lut.clear();
    int const x0 = std::max(0, static_cast<int>(std::floor(R.cx - R.r - 1)));
    int const x1 = std::min(FRAME_W - 1, static_cast<int>(std::ceil(R.cx + R.r + 1)));
    int const y0 = std::max(0, static_cast<int>(std::floor(R.cy - R.r - 1)));
    int const y1 = std::min(FRAME_H - 1, static_cast<int>(std::ceil(R.cy + R.r + 1)));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        float const nx = (x - R.cx) / R.r, ny = (y - R.cy) / R.r, r2 = nx * nx + ny * ny;
        if (r2 > 1.f) continue;
        float const nz = std::sqrt(std::max(1e-4f, 1.f - r2));
        float const lonS = std::atan2(nx, nz) / (2.f * PI);
        float const latS = std::asin(std::clamp(ny, -1.f, 1.f)) / PI + 0.5f;
        LutPixel px;
        px.x = x; px.y = y;
        px.u0 = _warp * lonS + (1.f - _warp) * (nx * _span * 0.5f);
        px.v = _warp * latS + (1.f - _warp) * (ny * 0.5f + 0.5f) + _voff;
        px.shade = 0.55f + 0.45f * nz;
        float hl = std::clamp(nz * 0.9f - nx * 0.25f - ny * 0.35f, 0.f, 1.f);
        px.hl = hl * hl * hl * 0.35f;
        px.alpha = std::clamp((1.f - std::sqrt(r2)) * R.r * 0.9f, 0.f, 1.f);
        _lut.push_back(px);
      }
    _lutRace = _raceIdx;
  }

  void TimeGlobeWidget::paintEvent(QPaintEvent*)
  {
    if (_lutRace != _raceIdx)
      buildLut();

    RaceDef const& R = _races[_raceIdx];
    float const minutes = currentMinutes();
    _lastMinutes = minutes;
    float const dt = minutes / 1440.f;
    float const scroll = std::fmod((minutes - 720.f) / 1440.f + 1.f, 1.f);
    bool const isDay = dt >= 0.25f && dt < 0.75f;
    float const prog = isDay ? (dt - 0.25f) / 0.5f
                             : ((dt < 0.25f ? dt + 0.25f : dt - 0.75f) / 0.5f);
    int const lit = std::clamp(static_cast<int>(std::lround(prog * 8.f)), 0, 8);
    QImage const& glowStud = isDay ? _glowStudDay : _glowStudNight;
    QImage const& glowHalo = isDay ? _glowHaloDay : _glowHaloNight;

    // 1. warp the day/night strip into the globe circle (native res)
    _globeImg.fill(Qt::transparent);
    for (LutPixel const& px : _lut)
    {
      float rr, gg, bb;
      sampleStrip(_strip, px.u0 + scroll, px.v, rr, gg, bb);
      int const r8 = std::min(255, static_cast<int>(rr * px.shade + px.hl * 255.f));
      int const g8 = std::min(255, static_cast<int>(gg * px.shade + px.hl * 255.f));
      int const b8 = std::min(255, static_cast<int>(bb * px.shade + px.hl * 255.f));
      _globeImg.setPixel(px.x, px.y, qRgba(r8, g8, b8, static_cast<int>(px.alpha * 255.f)));
    }

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QRectF const dst(0, 0, width(), height());
    float const sx = width() / static_cast<float>(FRAME_W);
    float const sy = height() / static_cast<float>(FRAME_H);

    // 2. globe, then 3. frame on top
    p.drawImage(dst, _globeImg, QRectF(0, 0, FRAME_W, FRAME_H));
    p.drawImage(dst, R.frame, QRectF(0, 0, FRAME_W, FRAME_H));

    // 4. rim halo (additive) -- only its bright ring shows; day gold / night blue
    if (_halo > 0.f)
    {
      float const hw = R.r * 2.f * _haloSz;
      QRectF const hr((R.cx - hw / 2) * sx, (R.cy - hw / 2) * sy, hw * sx, hw * sy);
      p.setCompositionMode(QPainter::CompositionMode_Plus);
      p.setOpacity(_halo);
      p.drawImage(hr, glowHalo, QRectF(QPointF(0, 0), glowHalo.size()));
    }

    // 5. stud loading bar (additive): first `lit` studs, clockwise from top
    if (_studGlow > 0.f && lit > 0)
    {
      static int const order[8] = {2, 1, 0, 7, 6, 5, 4, 3};
      std::array<QPointF, 8> studs;
      studPositions(R, studs);
      float const gs = 15.f;
      p.setCompositionMode(QPainter::CompositionMode_Plus);
      p.setOpacity(_studGlow);
      for (int n = 0; n < lit; ++n)
      {
        QPointF const s = studs[order[n]];
        QRectF const sr((s.x() - gs / 2) * sx, (s.y() - gs / 2) * sy, gs * sx, gs * sy);
        p.drawImage(sr, glowStud, QRectF(QPointF(0, 0), glowStud.size()));
      }
    }

    p.setOpacity(1.0);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  }

  void TimeGlobeWidget::ensurePopup()
  {
    if (_popup) return;
    _popup = new QWidget(this, Qt::Popup);
    // Horizontal layout: [ clock + time slider ] | [ Seasonal Events button ] on the RIGHT.
    auto lay = new QHBoxLayout(_popup);
    auto time_col = new QVBoxLayout();
    _clock = new QLabel("12:00", _popup);
    _clock->setAlignment(Qt::AlignCenter);
    _slider = new QSlider(Qt::Horizontal, _popup);
    _slider->setMinimum(0);
    _slider->setMaximum(1439); // minutes 0:00 .. 23:59
    _slider->setMinimumWidth(200);
    time_col->addWidget(_clock);
    time_col->addWidget(_slider);
    lay->addLayout(time_col);

    // Seasonal Events section, to the RIGHT of the time slider: a vertical separator then the calendar
    // button (built by MapView). Its menu flies out further to the right of the popup.
    if (_mapView)
    {
      auto sep = new QFrame(_popup);
      sep->setFrameShape(QFrame::VLine);
      sep->setFrameShadow(QFrame::Sunken);
      lay->addWidget(sep);
      lay->addWidget(_mapView->makeSeasonalEventsToolButton(_popup), 0, Qt::AlignVCenter);
    }

    auto fmt = [](int m) { return QString("%1:%2").arg(m / 60, 2, 10, QChar('0')).arg(m % 60, 2, 10, QChar('0')); };
    connect(_slider, &QSlider::valueChanged, this, [this, fmt](int minutes)
    {
      if (_mapView && _mapView->getWorld())
        _mapView->getWorld()->time = static_cast<float>(minutes * 2);
      _clock->setText(fmt(minutes));
      if (_mapView) _mapView->update();
      update();
    });
  }

  QRectF TimeGlobeWidget::hitRect() const
  {
    RaceDef const& R = _races[_raceIdx];
    float const sx = width() / static_cast<float>(FRAME_W);
    float const sy = height() / static_cast<float>(FRAME_H);
    float const half = R.studR + 14.f; // globe circle + studs + a little
    return QRectF((R.cx - half) * sx, (R.cy - half) * sy, 2.f * half * sx, 2.f * half * sy);
  }

  void TimeGlobeWidget::mouseMoveEvent(QMouseEvent* event)
  {
    // hand cursor only over the clickable orb; normal arrow over the ornament/wings
    setCursor(hitRect().contains(event->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
  }

  void TimeGlobeWidget::mousePressEvent(QMouseEvent* event)
  {
    // Only the orb is clickable -- clicks on the frame's ornament/wings do nothing, so only the globe
    // opens the time slider.
    if (!hitRect().contains(event->pos()))
    {
      event->ignore();
      return;
    }

    ensurePopup();
    int const minutes = static_cast<int>(currentMinutes());
    { QSignalBlocker const b(_slider); _slider->setValue(minutes); }
    _clock->setText(QString("%1:%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0')));
    QPoint const pos = mapToGlobal(QPoint(width() / 2 - 120, height()));
    _popup->move(pos);
    _popup->show();
  }
}
