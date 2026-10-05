#include "Services.hpp"
#include <noggit/DBC.h>
#include <QFileInfo>
namespace Noggit::Creator {
QString AppearanceService::raceName(int race) {
  static QStringList names{"Other","Human","Orc","Dwarf","Night Elf","Undead","Tauren","Gnome","Troll","Goblin","High Elf","Draenei"};
  return race>=0&&race<names.size()?names[race]:"Other";
}
QVector<Appearance> AppearanceService::catalog() {
  QVector<Appearance> result; QMap<QString,int> variants;
  for(auto record:gCreatureDisplayInfoDB) {
    try {
      Appearance a; a.display=record.getUInt(CreatureDisplayInfoDB::ID);
      auto model=gCreatureModelDataDB.getByID(record.getUInt(CreatureDisplayInfoDB::ModelID));
      a.model=QString::fromUtf8(model.getString(CreatureModelDataDB::ModelName));
      a.model.replace('\\','/'); if(a.model.endsWith(".mdx",Qt::CaseInsensitive)) a.model.chop(4),a.model+=".m2";
      auto extra=record.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if(extra) {
        auto e=gCreatureDisplayInfoExtraDB.getByID(extra); a.race=e.getUInt(1); a.sex=e.getUInt(2);
        for(int i=0;i<5;++i) a.features[i]=e.getUInt(i+3);
        a.name=raceName(a.race)+(a.sex==0?" Male":" Female");
      } else a.name=QFileInfo(a.model).baseName();
      a.name+=" · Look "+QString::number(++variants[a.name]); result.push_back(a);
    } catch(DBCFile::NotFound const&) {}
  }
  return result;
}
QVector<Choice> AppearanceService::factions() {
  QVector<Choice> result;
  for(auto row:gFactionTemplateDB) {
    auto id=row.getUInt(FactionTemplateDB::ID); QString name;
    try { auto faction=gFactionDB.getByID(row.getUInt(FactionTemplateDB::Faction)); name=QString::fromUtf8(faction.getLocalizedString(FactionDB::Name)); }
    catch(DBCFile::NotFound const&) {}
    if(name.isEmpty()) name=id==35?"Friendly":id==7?"Neutral":id==14?"Hostile":"Unnamed faction";
    auto hostile=row.getUInt(FactionTemplateDB::HostileMask);
    QString detail=(hostile&6)==6?"Hostile to Alliance and Horde":hostile&2?"Hostile to Alliance":hostile&4?"Hostile to Horde":"No hostile faction mask";
    result.push_back({id,name,detail});
  }
  return result;
}
}
