#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <ui_NoggitProjectCreationDialog.h>
#include <QFileDialog>
#include <QSettings>
#include <QMessageBox>
#include <QComboBox>
#include <QLabel>
#include <QFormLayout>

#include <noggit/casc/BuildInfo.hpp>

#include <filesystem>

NoggitProjectCreationDialog::NoggitProjectCreationDialog(ProjectInformation& project_information, QWidget* parent)
    : QDialog(parent)
    , ui(new ::Ui::NoggitProjectCreationDialog)
    , _project_information(project_information)
{
  setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

  ui->setupUi(this);

  QIcon icon = QIcon(":/icon-classic");
  ui->expansion_icon->setPixmap(icon.pixmap(QSize(32, 32)));
  ui->expansion_icon->setObjectName("icon");
  ui->expansion_icon->setStyleSheet("QLabel#icon { padding: 0px }");

  // Modern CASC clients (shared store, one product per build). The product combo is filled from the
  // client folder's .build.info and only enabled for these versions.
  ui->project_expansion->addItem("Classic Era");
  ui->project_expansion->addItem("Anniversary");

  auto* product_combo = new QComboBox(this);
  product_combo->setMinimumHeight(32);
  product_combo->setToolTip("Product of a shared CASC install (.build.info), e.g. wow_classic_era or wow_anniversary");
  ui->formLayout->addRow(new QLabel("CASC product", this), product_combo);

  auto const is_casc_version = [](std::string const& version)
  {
    return version == "Classic Era" || version == "Anniversary" || version == "Shadowlands";
  };

  auto const refresh_products = [this, product_combo, is_casc_version]()
  {
    auto const version_selected = ui->project_expansion->currentText().toStdString();
    product_combo->clear();
    product_combo->setEnabled(is_casc_version(version_selected));
    if (!product_combo->isEnabled())
      return;

    std::string const root = Noggit::Casc::findStorageRoot(ui->clientPathField->text().toStdString());
    if (root.empty())
    {
      product_combo->addItem("(no .build.info under the client path)", QString());
      return;
    }

    std::string const wanted = version_selected == "Classic Era" ? "wow_classic_era"
                             : version_selected == "Anniversary" ? "wow_anniversary" : "wow";
    int wanted_index = -1;
    for (auto const& product : Noggit::Casc::readBuildInfo(root))
    {
      product_combo->addItem(QString::fromStdString(product.code + "  (" + product.version + ")"),
                             QString::fromStdString(product.code));
      if (product.code == wanted)
        wanted_index = product_combo->count() - 1;
    }
    if (product_combo->count() == 0)
      product_combo->addItem("(no products listed in .build.info)", QString());
    if (wanted_index >= 0)
      product_combo->setCurrentIndex(wanted_index);
  };

  QObject::connect(ui->project_expansion, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, refresh_products](int index)
                   {
                     auto version_selected = ui->project_expansion->currentText().toStdString();

                     QIcon icon;
                     if (version_selected == "Turtle WoW" || version_selected == "Vanilla" || version_selected == "Classic Era")
                       icon = QIcon(":/icon-classic");
                     else if (version_selected == "Anniversary")
                       icon = QIcon(":/icon-burning");
                     else if (version_selected == "Wrath Of The Lich King")
                       icon = QIcon(":/icon-wrath");
                     else if (version_selected == "Shadowlands")
                       icon = QIcon(":/icon-shadow");
                     else if (version_selected == "Classic Era")
                       icon = QIcon(":/icon-classic");
                     else if (version_selected == "Anniversary")
                       icon = QIcon(":/icon-burning");

                     ui->expansion_icon->setPixmap(icon.pixmap(QSize(32, 32)));
                     refresh_products();
                   }
  );
  QObject::connect(ui->clientPathField, &QLineEdit::textChanged, [refresh_products](QString const&) { refresh_products(); });
  refresh_products();

  QObject::connect(ui->clientPathField_browse, &QPushButton::clicked, [this]
                   {
                     // TODO: implement automatic client path detection
                     QSettings settings;
                     auto default_path = settings.value("project/game_path").toString();
                     ui->clientPathField->setText(default_path);

                     QString folder_name = QFileDialog::getExistingDirectory(this, "Select Client Directory", default_path,
                                                                             QFileDialog::ShowDirsOnly |
                                                                             QFileDialog::DontResolveSymlinks);
                     ui->clientPathField->setText(folder_name);
                   }
  );

  QObject::connect(ui->projectPathField_browse, &QPushButton::clicked, [this]
                   {
                     QString folder_name = QFileDialog::getExistingDirectory(this, "Select Project Directory", "/",
                                                                             QFileDialog::ShowDirsOnly |
                                                                             QFileDialog::DontResolveSymlinks);
                     ui->projectPathField->setText(folder_name);
                   }
  );

  QObject::connect(ui->button_ok, &QPushButton::clicked, [&, product_combo, is_casc_version]
                   {
                     project_information.project_name = ui->projectName->text().toStdString();

                     if (project_information.project_name.empty())
                     {
                       QMessageBox::critical(this, "Error", "Project must have a name.");
                       return;
                     }

                     project_information.game_client_path = ui->clientPathField->text().toStdString();

                     if (project_information.game_client_path.empty())
                     {
                       QMessageBox::critical(this, "Error", "Game client path is empty.");
                       return;
                     }

                     std::filesystem::path game_path(project_information.game_client_path);
                     if (!std::filesystem::exists(game_path))
                     {
                       QMessageBox::critical(this, "Error", "Game client path does not exist.");
                       return;
                     }

                     project_information.project_path = ui->projectPathField->text().toStdString();

                     std::filesystem::path project_path(project_information.project_path);

                     if (project_path.empty())
                     {
                       QMessageBox::critical(this, "Error", "Project path is empty.");
                       return;
                     }

                     project_information.game_client_version = ui->project_expansion->currentText().toStdString();
                     project_information.casc_product = product_combo->isEnabled()
                                                      ? product_combo->currentData().toString().toStdString()
                                                      : std::string();

                     if (is_casc_version(project_information.game_client_version) && project_information.casc_product.empty())
                     {
                       QMessageBox::critical(this, "Error", "No CASC product could be read from the client path (.build.info).");
                       return;
                     }

                     done(QDialog::Accepted);
                     close();
                   }
  );

  QObject::connect(ui->button_cancel, &QPushButton::clicked, [&]
                   {
                     done(QDialog::Rejected);
                     close();
                   }
  );
}

NoggitProjectCreationDialog::~NoggitProjectCreationDialog()
{
  delete ui;
}
