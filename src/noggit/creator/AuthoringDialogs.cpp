#include "AuthoringDialogs.hpp"
#include <noggit/World.h>
#include <noggit/ui/tools/AssetBrowser/ModelView.hpp>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <QTimer>
#include <QSignalBlocker>
#include <functional>
#include <optional>

namespace Noggit::Creator {
namespace {
void error(QWidget* parent, std::exception const& e) { QMessageBox::warning(parent,"Creator",QString::fromUtf8(e.what())); }
QLabel* note(QString text,QWidget* parent) { auto l=new QLabel(text,parent); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l; }
QSpinBox* spin(QFormLayout* form,QString name,int value,int minimum,int maximum) { auto s=new QSpinBox; s->setRange(minimum,maximum); s->setValue(value); form->addRow(name,s); return s; }
std::optional<Choice> choose(QWidget* parent,QString title,std::function<QVector<Choice>(QString)> search) {
  QDialog dialog(parent); dialog.setWindowTitle(title); dialog.resize(560,500); auto layout=new QVBoxLayout(&dialog);
  auto input=new QLineEdit; input->setPlaceholderText("Search by name…"); layout->addWidget(input);
  auto list=new QListWidget; layout->addWidget(list); auto status=note("",&dialog); layout->addWidget(status);
  auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); layout->addWidget(buttons);
  QVector<Choice> results; QTimer timer; timer.setSingleShot(true); timer.setInterval(250);
  auto refresh=[&] { try { results=search(input->text()); list->clear(); for(auto const& r:results) list->addItem(r.name+(r.detail.isEmpty()?"":"\n"+r.detail)); status->setText(results.size()==250?"Showing the first 250 matches. Refine your search.":QString()); } catch(std::exception const& e) { results.clear(); list->clear(); status->setText(e.what()); } };
  QObject::connect(input,&QLineEdit::textChanged,&timer,[&]{timer.start();}); QObject::connect(&timer,&QTimer::timeout,&dialog,refresh);
  QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(list->currentRow()>=0)dialog.accept();});
  QObject::connect(list,&QListWidget::itemDoubleClicked,&dialog,[&]{dialog.accept();});
  QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); refresh();
  if(dialog.exec()!=QDialog::Accepted||list->currentRow()<0) return {}; return results[list->currentRow()];
}
class AppearancePreview : public Ui::Tools::AssetBrowser::ModelViewer {
  World* _world; World::CreatureSpawnOverlay _spawn; bool _pending=false; QString _requested;
public:
  AppearancePreview(World* world,QWidget* parent):ModelViewer(parent,Noggit::ASSET_BROWSER_PREVIEW),_world(world) { setMinimumSize(240,260); }
  void appearance(Appearance const& a) {
    _spawn.display_id=a.display; _spawn.is_character_model=a.race!=0; _pending=true;
    _requested=a.model;
  }
protected:
  void draw() override {
    if (!_requested.isEmpty()) { auto path=_requested;_requested.clear();setModel(path.toStdString()); }
    if(_pending&&!_model_instances.empty()&&_model_instances[0].model->finishedLoading()) {
      _world->applyCreatureSpawnModelAppearance(_spawn,_model_instances[0],Noggit::ASSET_BROWSER_PREVIEW); _pending=false;
    }
    PreviewRenderer::draw();
  }
};

bool npcDialog(QWidget* parent,World* world,Npc draft,int mode,Id* saved) {
  QDialog dialog(parent); dialog.setWindowTitle(draft.entry?"Edit NPC":"Create NPC"); dialog.resize(850,740);
  auto layout=new QVBoxLayout(&dialog); auto name=new QLineEdit(draft.name); name->setPlaceholderText("NPC name"); layout->addWidget(name);
  auto tabs=new QTabWidget; layout->addWidget(tabs);
  auto appearancePage=new QWidget; auto appearanceLayout=new QHBoxLayout(appearancePage); auto controls=new QVBoxLayout; appearanceLayout->addLayout(controls,1);
  auto appearanceForm=new QFormLayout; controls->addLayout(appearanceForm);
  auto search=new QLineEdit; search->setPlaceholderText("Search models / existing looks…"); appearanceForm->addRow("Find look",search);
  auto race=new QComboBox,sex=new QComboBox; sex->addItems({"Male","Female"});
  auto looks=new QComboBox; appearanceForm->addRow("Race",race); appearanceForm->addRow("Sex",sex); appearanceForm->addRow("Existing look",looks);
  auto catalog=AppearanceService::catalog();
  QMap<int,bool> races; for(auto const& a:catalog) if(a.race) races[a.race]=true;
  for(auto r:races.keys()) race->addItem(AppearanceService::raceName(r),r);
  std::array<QComboBox*,5> features; QStringList labels{"Skin","Face","Hair Style","Hair Color","Facial Hair"};
  for(int i=0;i<5;++i) { features[i]=new QComboBox; appearanceForm->addRow(labels[i],features[i]); }
  auto showHumanoid=[&](bool visible) {
    for(auto field:{race,sex}) {field->setVisible(visible);appearanceForm->labelForField(field)->setVisible(visible);}
    for(auto field:features) {field->setVisible(visible);appearanceForm->labelForField(field)->setVisible(visible);}
  };
  bool humanoid=mode!=2; showHumanoid(humanoid);
  auto preview=new AppearancePreview(world,&dialog); appearanceLayout->addWidget(preview,1);
  controls->addWidget(note("Each look is a valid existing client appearance. Armor is part of the look; changing a feature selects the closest available combination. Preview shows body textures; attached armor and weapons are verified in the world.",&dialog));
  auto equipmentGroup=new QGroupBox("Visible equipment"); auto equipmentForm=new QFormLayout(equipmentGroup); controls->addWidget(equipmentGroup);
  std::array<QPushButton*,3> weapons; QStringList slotNames{"Main Hand","Off Hand","Ranged"};
  auto refreshWeapons=[&]{for(int i=0;i<3;++i) weapons[i]->setText(EquipmentService::name(draft.equipment[i]));};
  for(int i=0;i<3;++i) {
    weapons[i]=new QPushButton; equipmentForm->addRow(slotNames[i],weapons[i]);
    QObject::connect(weapons[i],&QPushButton::clicked,&dialog,[&,i]{try {
      auto item=choose(&dialog,"Choose "+slotNames[i],[i](QString text){auto choices=EquipmentService::search(text,i); choices.prepend({0,"None","Remove this visible item"}); return choices;});
      if(item) {draft.equipment[i]=item->id;refreshWeapons();}
    }catch(std::exception const& e){error(&dialog,e);}});
  }
  refreshWeapons();
  auto outfitButtons=new QHBoxLayout; auto applyOutfit=new QPushButton("Apply Outfit / Existing NPC Look"); auto saveOutfit=new QPushButton("Save Current Outfit As…");
  outfitButtons->addWidget(applyOutfit);outfitButtons->addWidget(saveOutfit); controls->addLayout(outfitButtons);
  tabs->addTab(appearancePage,"Appearance / Equipment");
  int selected=-1; bool updating=false;
  auto selectLook=[&](int index) {
    if(index<0||index>=catalog.size()) return; selected=index; auto const& a=catalog[index]; draft.display=a.display;
    updating=true;
    {QSignalBlocker b(race);race->setCurrentIndex(race->findData(a.race));} {QSignalBlocker b(sex);sex->setCurrentIndex(a.sex==0?0:1);}
    for(int i=0;i<5;++i) {
      QSignalBlocker b(features[i]);features[i]->clear();QMap<int,bool> values;
      for(auto const& candidate:catalog) if(candidate.race==a.race&&candidate.sex==a.sex) values[candidate.features[i]]=true;
      for(auto v:values.keys()) features[i]->addItem(QString::number(v+1),v);
      features[i]->setCurrentIndex(features[i]->findData(a.features[i]));
    }
    updating=false; showHumanoid(a.race!=0); preview->appearance(a);
  };
  auto populate=[&] {
    QSignalBlocker block(looks); looks->clear();
    for(int i=0;i<catalog.size();++i) { auto const& a=catalog[i];
      if(humanoid?(a.race!=race->currentData().toInt()||a.sex!=sex->currentIndex()):a.race!=0) continue;
      if(!a.name.contains(search->text(),Qt::CaseInsensitive)&&!a.model.contains(search->text(),Qt::CaseInsensitive)) continue;
      looks->addItem(a.name,i);
    }
    auto found=looks->findData(selected); looks->setCurrentIndex(found>=0?found:0);
    if(looks->currentIndex()>=0) selectLook(looks->currentData().toInt());
  };
  QObject::connect(looks,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int i){if(i>=0)selectLook(looks->itemData(i).toInt());});
  for(auto combo:{race,sex}) QObject::connect(combo,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&]{if(!updating)populate();});
  QObject::connect(search,&QLineEdit::textChanged,&dialog,[&]{populate();});
  for(int f=0;f<5;++f) QObject::connect(features[f],QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&,f]{
    if(updating||selected<0) return; auto current=catalog[selected];int best=-1,score=-1;
    for(int i=0;i<catalog.size();++i) {auto const& a=catalog[i];if(a.race!=current.race||a.sex!=current.sex||a.features[f]!=features[f]->currentData().toInt())continue;int matches=0;for(int j=0;j<5;++j)if(a.features[j]==current.features[j])++matches;if(matches>score){score=matches;best=i;}}
    if(best>=0){selectLook(best);populate();}
  });
  QObject::connect(applyOutfit,&QPushButton::clicked,&dialog,[&]{try{
    auto saved=EquipmentService::outfits();
    auto choice=choose(&dialog,"Apply outfit",[&](QString text){QVector<Choice> out;for(int i=0;i<saved.size();++i)if(saved[i].name.contains(text,Qt::CaseInsensitive))out.push_back({Id(i+1),saved[i].name,"Saved outfit"}); auto existing=CreatureService::search(text);for(auto c:existing){c.id|=0x80000000u;c.detail="Existing NPC appearance";out.push_back(c);}return out;});
    if(!choice)return;Id display;
    if(choice->id&0x80000000u){auto npc=CreatureService::load(choice->id&0x7fffffffu);display=npc.display;draft.equipment=npc.equipment;}
    else{auto o=saved[choice->id-1];display=o.display;draft.equipment=o.equipment;}
    for(int i=0;i<catalog.size();++i)if(catalog[i].display==display){humanoid=catalog[i].race!=0;showHumanoid(humanoid);selectLook(i);search->clear();populate();break;}refreshWeapons();
  }catch(std::exception const& e){error(&dialog,e);}});
  QObject::connect(saveOutfit,&QPushButton::clicked,&dialog,[&]{bool ok=false;auto title=QInputDialog::getText(&dialog,"Save outfit","Outfit name",QLineEdit::Normal,{},&ok);if(ok)try{EquipmentService::saveOutfit({title,draft.display,draft.equipment});}catch(std::exception const& e){error(&dialog,e);}});
  for(int i=0;i<catalog.size();++i) if(catalog[i].display==draft.display){selected=i;humanoid=catalog[i].race!=0;break;}
  if(selected>=0){updating=true;race->setCurrentIndex(race->findData(catalog[selected].race));sex->setCurrentIndex(catalog[selected].sex);updating=false;}
  populate();
  auto combatPage=new QWidget; auto combatLayout=new QVBoxLayout(combatPage); auto form=new QFormLayout;combatLayout->addLayout(form);
  combatLayout->addWidget(note("Combat values are independent of visible equipment. Class profiles provide starting values, not spellcasting AI. Vendor and Trainer roles mark the NPC only; inventories and lessons are outside this stage.",&dialog));
  auto klass=new QComboBox; klass->addItems({"Warrior","Mage","Rogue","Paladin"}); form->addRow("Class profile",klass);
  auto power=new QLabel;form->addRow("Resource",power);
  auto level=spin(form,"Level",draft.level,1,63);auto health=spin(form,"Health",draft.health,1,100000000);auto mana=spin(form,"Mana",draft.mana,0,100000000);
  auto faction=new QComboBox; auto factions=AppearanceService::factions();
  for(auto const& f:factions)faction->addItem(f.name+" — "+f.detail,f.id);faction->setCurrentIndex(faction->findData(draft.faction));form->addRow("Faction",faction);
  auto reaction=new QComboBox;reaction->addItems({"Use selected faction","Friendly to both sides","Neutral","Hostile to both sides"});form->addRow("Disposition",reaction);
  QObject::connect(reaction,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int i){if(i>0)faction->setCurrentIndex(faction->findData(i==1?35:i==2?7:14));});
  auto role=new QComboBox;role->addItems({"Normal NPC","Quest Giver","Vendor","Trainer","Enemy","Elite","Rare","Guard"});role->setCurrentIndex(draft.role);form->addRow("Role",role);
  auto rank=new QComboBox;rank->addItems({"Normal","Elite","Rare Elite","Boss","Rare"});rank->setCurrentIndex(draft.rank);form->addRow("Rank",rank);
  auto type=new QComboBox; QStringList types{"Other","Beast","Dragonkin","Demon","Elemental","Giant","Undead","Humanoid","Critter","Mechanical"}; type->addItems(types);type->setCurrentIndex(draft.type);form->addRow("Creature type",type);
  auto respawn=spin(form,"Respawn (seconds)",draft.respawn,1,604800);
  auto advanced=new QGroupBox("Advanced combat");advanced->setCheckable(true);advanced->setChecked(false);auto advancedForm=new QFormLayout(advanced);combatLayout->addWidget(advanced);
  auto armor=spin(advancedForm,"Armor",draft.armor,0,16777215);auto attack=spin(advancedForm,"Attack interval (milliseconds)",std::max(100,draft.attackMs),100,60000);
  auto damageMin=new QDoubleSpinBox,damageMax=new QDoubleSpinBox;for(auto s:{damageMin,damageMax})s->setRange(0,10000000);damageMin->setValue(draft.damageMin);damageMax->setValue(draft.damageMax);advancedForm->addRow("Minimum damage",damageMin);advancedForm->addRow("Maximum damage",damageMax);
  auto movement=new QComboBox;movement->addItems({"Stay here","Wander nearby"});movement->setCurrentIndex(draft.movement==1?1:0);advancedForm->addRow("Movement",movement);
  auto classDefaults=[&]{int c=klass->currentIndex();draft.unitClass=c==0?1:c==1?8:c==2?4:2;power->setText(draft.mana>0?"Mana":c==2?"Energy":"Rage");};
  {QSignalBlocker b(klass);klass->setCurrentIndex(draft.unitClass==8?1:draft.unitClass==4?2:draft.unitClass==2?3:0);}classDefaults();
  QObject::connect(klass,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&]{classDefaults();int l=level->value();health->setValue(l*(klass->currentIndex()==1?55:85));mana->setValue((klass->currentIndex()==1||klass->currentIndex()==3)?l*65:0);damageMin->setValue(l*1.5);damageMax->setValue(l*2.5);});
  QObject::connect(mana,QOverload<int>::of(&QSpinBox::valueChanged),&dialog,[&](int value){power->setText(value>0?"Mana":draft.unitClass==4?"Energy":"Rage");});
  mana->setToolTip("Mana above zero uses Mana. With zero Mana, Rogue uses Energy and other profiles use Rage.");
  QObject::connect(role,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int r){if(r==5)rank->setCurrentIndex(1);if(r==6)rank->setCurrentIndex(4);if(r==4)reaction->setCurrentIndex(3);});
  tabs->addTab(combatPage,"Combat / Role");
  auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);
  QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(draft.entry) {
    auto remove=buttons->addButton("Delete NPC…",QDialogButtonBox::DestructiveRole);
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&]{
      if(QMessageBox::question(&dialog,"Delete NPC","Delete \""+draft.name+"\" and all of its placements from your local world?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
      try{CreatureService::remove(draft.entry);dialog.accept();}catch(std::exception const& e){error(&dialog,e);}
    });
  }
  QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{try {
    draft.name=name->text();draft.level=level->value();draft.health=health->value();draft.mana=mana->isEnabled()?mana->value():0;draft.armor=armor->value();draft.attackMs=attack->value();draft.damageMin=damageMin->value();draft.damageMax=damageMax->value();draft.faction=faction->currentData().toUInt();draft.rank=rank->currentIndex();draft.role=role->currentIndex();draft.type=type->currentIndex();draft.respawn=respawn->value();draft.movement=movement->currentIndex();
    auto entry=CreatureService::save(draft);if(saved)*saved=entry;dialog.accept();
  }catch(std::exception const& e){error(&dialog,e);}});
  return dialog.exec()==QDialog::Accepted;
}
}
std::optional<Id> createNpc(QWidget* parent,World* world,NpcKind kind,Id source) {
  try {
    int mode=int(kind);Npc draft; if(kind==NpcKind::Creature)draft.type=1;
    if(kind==NpcKind::Clone) {
      if(!source){auto chosen=choose(parent,"Clone Existing NPC",[](QString text){return CreatureService::search(text);});if(!chosen)return {};source=chosen->id;}
      auto original=CreatureService::load(source);
      QDialog options(parent);options.setWindowTitle("Clone "+original.name);auto form=new QVBoxLayout(&options);
      form->addWidget(note("The original NPC will remain unchanged. Choose what to copy:",&options));
      std::array<QCheckBox*,6> copy;QStringList groups{"Appearance","Base stats","Faction","Equipment","Basic combat configuration","Movement defaults"};
      for(int i=0;i<6;++i){copy[i]=new QCheckBox(groups[i]);copy[i]->setChecked(true);form->addWidget(copy[i]);}
      std::array<QCheckBox*,5> relations;QStringList relationNames{"Loot","Vendor inventory","Trainer spells","Gossip","Quests"};
      for(int i=0;i<5;++i){relations[i]=new QCheckBox(relationNames[i]);form->addWidget(relations[i]);}
      auto scripts=new QCheckBox("AI / scripts");scripts->setEnabled(false);scripts->setToolTip("Scripts may depend on the original NPC identity and cannot be safely cloned in this stage.");form->addWidget(scripts);
      form->addWidget(note("Optional associations reuse existing game definitions. They do not create separate editable loot, vendor, trainer or gossip systems.",&options));
      form->addWidget(note(QString("Level %1 · %2\nMain Hand: %3\nOff Hand: %4\nRanged: %5").arg(original.level).arg(original.type==7?"Humanoid":"Creature").arg(EquipmentService::name(original.equipment[0])).arg(EquipmentService::name(original.equipment[1])).arg(EquipmentService::name(original.equipment[2])),&options));
      QString factionName="Unknown faction",lookName="Existing client look";
      for(auto const& f:AppearanceService::factions())if(f.id==original.faction){factionName=f.name;break;}
      for(auto const& a:AppearanceService::catalog())if(a.display==original.display){lookName=a.name;break;}
      QStringList roles{"Normal NPC","Quest Giver","Vendor","Trainer","Enemy","Elite","Rare","Guard"};
      form->addWidget(note("Faction: "+factionName+"\nAppearance: "+lookName+"\nRole: "+roles.value(original.role),&options));
      auto only=new QPushButton("Clone Appearance Only");form->addWidget(only);
      QObject::connect(only,&QPushButton::clicked,&options,[&]{for(int i=0;i<6;++i)copy[i]->setChecked(i==0||i==3);for(auto r:relations)r->setChecked(false);options.accept();});
      auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addWidget(buttons);
      QObject::connect(buttons,&QDialogButtonBox::accepted,&options,&QDialog::accept);QObject::connect(buttons,&QDialogButtonBox::rejected,&options,&QDialog::reject);
      if(options.exec()!=QDialog::Accepted)return {};
      draft.source=source;draft.name=original.name+" Copy";
      draft.appearance=copy[0]->isChecked();draft.stats=copy[1]->isChecked();draft.allegiance=copy[2]->isChecked();draft.weapons=copy[3]->isChecked();draft.combat=copy[4]->isChecked();draft.motion=copy[5]->isChecked();
      draft.loot=relations[0]->isChecked();draft.vendor=relations[1]->isChecked();draft.trainer=relations[2]->isChecked();draft.gossip=relations[3]->isChecked();draft.quests=relations[4]->isChecked();
      if(draft.appearance){draft.display=original.display;draft.type=original.type;}
      if(draft.stats){draft.level=original.level;draft.health=std::max(1,original.health);draft.mana=original.mana;draft.armor=original.armor;draft.rank=original.rank;}
      if(draft.allegiance)draft.faction=original.faction;
      if(draft.weapons)draft.equipment=original.equipment;
      if(draft.combat){draft.unitClass=original.unitClass;draft.damageMin=original.damageMin;draft.damageMax=original.damageMax;draft.attackMs=std::max(100,original.attackMs);}
      if(draft.motion)draft.movement=original.movement==1?1:0;
      mode=original.type==7?1:2;
    }
    Id entry=0;
    if(!npcDialog(parent,world,draft,mode,&entry))return {};
    return entry;
  }catch(std::exception const& e){error(parent,e);return {};}
}
bool editNpc(QWidget* parent,World* world,Id entry) {
  try { if(!CreatureService::owned(entry)){QMessageBox::information(parent,"NPC properties","This is an original NPC. Clone it to make your own version.");return false;}auto npc=CreatureService::load(entry);return npcDialog(parent,world,npc,npc.type==7?1:2,nullptr); }
  catch(std::exception const& e){error(parent,e);return false;}
}
}
