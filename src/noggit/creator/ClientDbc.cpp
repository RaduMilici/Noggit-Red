#include "ClientDbc.hpp"
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
quint32 bits(float value) { quint32 out; std::memcpy(&out, &value, 4); return out; }
float fromBits(quint32 value) { float out; std::memcpy(&out, &value, 4); return out; }
QString trimmed(double value) { return QString::number(value, 'f', value == std::floor(value) ? 0 : 1); }
}
Wdbc Wdbc::parse(QByteArray const& bytes) {
  require(bytes.size() >= 20 && bytes.startsWith("WDBC"), "This is not a client table (WDBC) file.");
  auto u32 = [&](int at) { return qFromLittleEndian<quint32>(bytes.constData() + at); };
  quint32 rows = u32(4), columns = u32(8), rowSize = u32(12), stringSize = u32(16);
  require(columns && quint64(rowSize) == quint64(columns) * 4 && 20 + quint64(rows) * rowSize + stringSize == quint64(bytes.size()), "The client table is damaged or has an unexpected layout.");
  Wdbc table; table._columns = columns;
  table._records.resize(std::size_t(rows) * columns);
  for (std::size_t i = 0; i < table._records.size(); ++i) table._records[i] = u32(20 + int(i) * 4);
  table._strings = bytes.mid(20 + int(rows * rowSize), int(stringSize));
  if (table._strings.isEmpty()) table._strings = QByteArray(1, '\0');
  return table;
}
QByteArray Wdbc::bytes() const {
  QByteArray out("WDBC");
  auto append = [&](quint32 value) { char buffer[4]; qToLittleEndian(value, buffer); out.append(buffer, 4); };
  append(quint32(rows())); append(quint32(_columns)); append(quint32(_columns * 4)); append(quint32(_strings.size()));
  for (auto cell : _records) append(cell);
  out.append(_strings);
  return out;
}
float Wdbc::real(int row, int column) const { return fromBits(cell(row, column)); }
QString Wdbc::text(int row, int column) const {
  auto offset = cell(row, column);
  if (offset >= quint32(_strings.size())) return {};
  return QString::fromUtf8(_strings.constData() + offset);
}
int Wdbc::find(quint32 id) const {
  for (int r = 0; r < rows(); ++r) if (cell(r, 0) == id) return r;
  return -1;
}
quint32 Wdbc::string(QString const& text) {
  if (text.isEmpty()) return 0;
  if (auto it = _offsets.find(text); it != _offsets.end()) return *it;
  quint32 offset = quint32(_strings.size());
  _strings.append(text.toUtf8()); _strings.append('\0');
  _offsets.insert(text, offset);
  return offset;
}
void Wdbc::put(QVector<quint32> const& record, QHash<int, QString> const& strings) {
  require(std::size_t(record.size()) == _columns && !record.isEmpty(), "A client table record has the wrong number of columns.");
  auto values = record;
  for (auto it = strings.begin(); it != strings.end(); ++it) values[it.key()] = string(it.value());
  int row = find(values[0]);
  if (row < 0) { _records.insert(_records.end(), values.begin(), values.end()); return; }
  std::copy(values.begin(), values.end(), _records.begin() + std::ptrdiff_t(row) * std::ptrdiff_t(_columns));
}
void Wdbc::remove(quint32 id) {
  int row = find(id);
  if (row >= 0) _records.erase(_records.begin() + std::ptrdiff_t(row) * std::ptrdiff_t(_columns), _records.begin() + std::ptrdiff_t(row + 1) * std::ptrdiff_t(_columns));
}
namespace SpellDbc {
QVector<quint32> record(Fields const& spell, QHash<int, QString>& strings) {
  QVector<quint32> r(columns, 0);
  auto integer = [&](int column, QString const& name) { r[column] = quint32(spell.value(name).toLongLong() & 0xffffffffLL); };
  auto real = [&](int column, QString const& name) { r[column] = bits(spell.value(name).toFloat()); };
  static char const* const head[] = {"entry", "school", "category", "castUI", "dispel", "mechanic", "attributes", "attributesEx", "attributesEx2",
    "attributesEx3", "attributesEx4", "stances", "stancesNot", "targets", "targetCreatureType", "requiresSpellFocus", "casterAuraState",
    "targetAuraState", "castingTimeIndex", "recoveryTime", "categoryRecoveryTime", "interruptFlags", "auraInterruptFlags",
    "channelInterruptFlags", "procFlags", "procChance", "procCharges", "maxLevel", "baseLevel", "spellLevel", "durationIndex", "powerType",
    "manaCost", "manCostPerLevel", "manaPerSecond", "manaPerSecondPerLevel", "rangeIndex"};
  for (int i = 0; i < 37; ++i) integer(i, head[i]);
  real(37, "speed"); integer(38, "modelNextSpell"); integer(39, "stackAmount"); integer(40, "totem1"); integer(41, "totem2");
  for (int k = 0; k < 8; ++k) { integer(42 + k, QString("reagent%1").arg(k + 1)); integer(50 + k, QString("reagentCount%1").arg(k + 1)); }
  integer(58, "equippedItemClass"); integer(59, "equippedItemSubClassMask"); integer(60, "equippedItemInventoryTypeMask");
  struct Group { int column; char const* name; bool real; };
  static Group const effects[] = {{61, "effect", false}, {64, "effectDieSides", false}, {67, "effectBaseDice", false}, {70, "effectDicePerLevel", true},
    {73, "effectRealPointsPerLevel", true}, {76, "effectBasePoints", false}, {79, "effectMechanic", false}, {82, "effectImplicitTargetA", false},
    {85, "effectImplicitTargetB", false}, {88, "effectRadiusIndex", false}, {91, "effectApplyAuraName", false}, {94, "effectAmplitude", false},
    {97, "effectMultipleValue", true}, {100, "effectChainTarget", false}, {103, "effectItemType", false}, {106, "effectMiscValue", false},
    {109, "effectTriggerSpell", false}, {112, "effectPointsPerComboPoint", true}};
  for (auto const& g : effects)
    for (int j = 0; j < 3; ++j) { auto name = QString(g.name) + QString::number(j + 1); if (g.real) real(g.column + j, name); else integer(g.column + j, name); }
  struct Single { int column; char const* name; };
  static Single const tail[] = {{115, "spellVisual1"}, {116, "spellVisual2"}, {117, "spellIconId"}, {118, "activeIconId"}, {119, "spellPriority"},
    {128, "nameFlags"}, {137, "nameSubtextFlags"}, {146, "descriptionFlags"}, {155, "auraDescriptionFlags"}, {156, "manaCostPercentage"},
    {157, "startRecoveryCategory"}, {158, "startRecoveryTime"}, {159, "maxTargetLevel"}, {160, "spellFamilyName"}, {163, "maxAffectedTargets"},
    {164, "dmgClass"}, {165, "preventionType"}, {166, "stanceBarOrder"}, {170, "minFactionId"}, {171, "minReputation"}, {172, "requiredAuraVision"}};
  for (auto const& s : tail) integer(s.column, s.name);
  for (int j = 0; j < 3; ++j) real(167 + j, QString("dmgMultiplier%1").arg(j + 1));
  auto family = spell.value("spellFamilyFlags").toULongLong();
  r[161] = quint32(family & 0xffffffffULL); r[162] = quint32(family >> 32);
  // Every locale shows the same text: the world database keeps one language.
  for (auto [column, name] : std::initializer_list<std::pair<int, char const*>>{{120, "name"}, {129, "nameSubtext"}, {138, "description"}, {147, "auraDescription"}})
    for (int l = 0; l < locales; ++l) strings[column + l] = spell.value(name).toString();
  return r;
}
}
namespace ClientLists {
QString seconds(double ms) {
  if (ms <= 0) return "Instant";
  double s = ms / 1000;
  if (s < 60) return trimmed(s) + " sec";
  if (s < 3600) { int m = int(s) / 60, rest = int(s) % 60; return QString("%1 min").arg(m) + (rest ? QString(" %1 sec").arg(rest) : QString()); }
  int h = int(s) / 3600, m = (int(s) % 3600) / 60; return QString("%1 hr").arg(h) + (m ? QString(" %1 min").arg(m) : QString());
}
QVector<ClientChoice> castTimes(Wdbc const& t) {
  QVector<ClientChoice> out;
  for (int r = 0; r < t.rows(); ++r) {
    double ms = qint32(t.cell(r, 1)); bool scaling = qint32(t.cell(r, 2)) != 0;
    out.push_back({t.cell(r, 0), (ms <= 0 ? QString("Instant") : seconds(ms) + " cast") + (scaling ? " (changes with level)" : QString()), ms});
  }
  std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.value != b.value ? a.value < b.value : a.id < b.id; });
  return out;
}
QVector<ClientChoice> durations(Wdbc const& t) {
  QVector<ClientChoice> out;
  for (int r = 0; r < t.rows(); ++r) {
    double ms = qint32(t.cell(r, 1)), max = qint32(t.cell(r, 3));
    QString label = ms < 0 ? QString("Until cancelled") : ms == 0 ? QString("None") : seconds(ms);
    if (max > ms && ms >= 0) label += " (up to " + seconds(max) + ")";
    out.push_back({t.cell(r, 0), label, ms < 0 ? 1e12 : ms});
  }
  std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.value != b.value ? a.value < b.value : a.id < b.id; });
  return out;
}
QVector<ClientChoice> ranges(Wdbc const& t) {
  QVector<ClientChoice> out;
  for (int r = 0; r < t.rows(); ++r) {
    double min = t.real(r, 1), max = t.real(r, 2);
    auto name = t.text(r, 4);
    QString yards = max <= 0 ? QString("self") : (min > 0 ? trimmed(min) + "–" : QString()) + trimmed(max) + " yd";
    out.push_back({t.cell(r, 0), (name.isEmpty() ? QString("Range") : name) + " (" + yards + ")", max});
  }
  std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.value != b.value ? a.value < b.value : a.id < b.id; });
  return out;
}
QVector<ClientChoice> radii(Wdbc const& t) {
  QVector<ClientChoice> out;
  for (int r = 0; r < t.rows(); ++r) out.push_back({t.cell(r, 0), trimmed(t.real(r, 1)) + " yd", t.real(r, 1)});
  std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.value != b.value ? a.value < b.value : a.id < b.id; });
  return out;
}
}
}
