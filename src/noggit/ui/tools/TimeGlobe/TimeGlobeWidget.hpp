// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#ifndef NOGGIT_UI_TOOLS_TIMEGLOBEWIDGET_HPP
#define NOGGIT_UI_TOOLS_TIMEGLOBEWIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtGui/QImage>
#include <QtGui/QColor>
#include <QtCore/QString>
#include <QtCore/QPoint>
#include <QtCore/QRect>

#include <array>
#include <vector>

class MapView;
class QTimer;
class QSlider;
class QLabel;

namespace Noggit::Ui
{
  // Warcraft-3 style time-of-day indicator: an ornate race frame with a spinning warped globe
  // (the day/night panorama), a rim glow, and 8 studs that fill like a loading bar toward the next
  // day/night shift (gold by day, blue by night). Drives / reads World::time. Rendered entirely with
  // QPainter (no extra GL context): a per-race sphere-warp LUT samples the globe strip each frame.
  class TimeGlobeWidget : public QWidget
  {
    Q_OBJECT
  public:
    explicit TimeGlobeWidget(MapView* mapView, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void setRace(QString const& raceKey); // "human" | "orc" | "nightelf" | "undead"

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

  private:
    QRectF hitRect() const; // the clickable box around the orb (widget coords)

    struct RaceDef
    {
      QString key;
      QImage frame;
      float cx, cy, r, studR;
    };
    struct LutPixel { int x, y; float u0, v, shade, hl, alpha; };

    void buildLut();
    void ensurePopup();
    float currentMinutes() const;
    void studPositions(RaceDef const& race, std::array<QPointF, 8>& out) const;

    MapView* _mapView;

    QImage _strip;                          // globe day/night panorama (256x128)
    QImage _glowStudDay, _glowStudNight;    // pre-tinted additive glows (colour, alpha = brightness)
    QImage _glowHaloDay, _glowHaloNight;

    std::vector<RaceDef> _races;
    int _raceIdx = 0;

    std::vector<LutPixel> _lut;
    int _lutRace = -1;
    QImage _globeImg;                       // native-res warp render target

    // locked from the preview
    QColor _dayCol {0xFF, 0xD4, 0x2A};
    QColor _nightCol {0x4F, 0xA6, 0xF5};
    float _warp = 0.55f, _span = 0.60f, _voff = -0.05f;
    float _halo = 0.75f, _haloSz = 1.55f, _studGlow = 0.90f;

    QTimer* _timer = nullptr;
    float _lastMinutes = -1.f;

    QWidget* _popup = nullptr;
    QSlider* _slider = nullptr;
    QLabel* _clock = nullptr;
  };
}

#endif // NOGGIT_UI_TOOLS_TIMEGLOBEWIDGET_HPP
