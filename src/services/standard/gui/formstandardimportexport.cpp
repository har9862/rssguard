// This file is part of RSS Guard.
//
// Copyright (C) 2011-2016 by Martin Rotter <rotter.martinos@gmail.com>
//
// RSS Guard is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RSS Guard is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with RSS Guard. If not, see <http://www.gnu.org/licenses/>.

#include "services/standard/gui/formstandardimportexport.h"

#include "services/abstract/category.h"
#include "services/standard/standardfeedsimportexportmodel.h"
#include "services/standard/standardserviceroot.h"
#include "core/feedsmodel.h"
#include "miscellaneous/application.h"
#include "gui/feedmessageviewer.h"
#include "gui/feedsview.h"
#include "gui/dialogs/formmain.h"
#include "exceptions/ioexception.h"


#include <QFileDialog>
#include <QTextStream>


FormStandardImportExport::FormStandardImportExport(StandardServiceRoot *service_root, QWidget *parent)
  : QDialog(parent), m_ui(new Ui::FormStandardImportExport), m_serviceRoot(service_root) {
  m_ui->setupUi(this);
  m_model = new FeedsImportExportModel(m_ui->m_treeFeeds);

  connect(m_model, SIGNAL(parsingStarted()), this, SLOT(onParsingStarted()));
  connect(m_model, SIGNAL(parsingFinished(int,int,bool)), this, SLOT(onParsingFinished(int,int,bool)));
  connect(m_model, SIGNAL(parsingProgress(int,int)), this, SLOT(onParsingProgress(int,int)));

  setWindowFlags(Qt::MSWindowsFixedSizeDialogHint | Qt::Dialog | Qt::WindowSystemMenuHint);

  m_ui->m_lblSelectFile->setStatus(WidgetWithStatus::Error, tr("No file is selected."), tr("No file is selected."));
  m_ui->m_buttonBox->button(QDialogButtonBox::Ok)->disconnect();
  m_ui->m_lblResult->setStatus(WidgetWithStatus::Warning, tr("No operation executed yet."), tr("No operation executed yet."));

  connect(m_ui->m_buttonBox->button(QDialogButtonBox::Ok), SIGNAL(clicked()), this, SLOT(performAction()));
  connect(m_ui->m_btnSelectFile, SIGNAL(clicked()), this, SLOT(selectFile()));
  connect(m_ui->m_btnCheckAllItems, SIGNAL(clicked()), m_model, SLOT(checkAllItems()));
  connect(m_ui->m_btnUncheckAllItems, SIGNAL(clicked()), m_model, SLOT(uncheckAllItems()));
}

FormStandardImportExport::~FormStandardImportExport() {
}

void FormStandardImportExport::setMode(const FeedsImportExportModel::Mode &mode) {
  m_model->setMode(mode);
  m_ui->m_progressBar->setVisible(false);

  switch (mode) {
    case FeedsImportExportModel::Export: {
      m_model->setRootItem(m_serviceRoot);
      m_model->checkAllItems();
      m_ui->m_treeFeeds->setModel(m_model);
      m_ui->m_treeFeeds->expandAll();
      m_ui->m_cmbRootNode->setVisible(false);
      m_ui->m_lblRootNode->setVisible(false);
      m_ui->m_groupFile->setTitle(tr("Destination file"));
      m_ui->m_groupFeeds->setTitle(tr("Source feeds && categories"));
      setWindowTitle(tr("Export feeds"));
      setWindowIcon(qApp->icons()->fromTheme(QSL("document-export")));
      break;
    }

    case FeedsImportExportModel::Import: {
      m_ui->m_groupFile->setTitle(tr("Source file"));
      m_ui->m_groupFeeds->setTitle(tr("Target feeds && categories"));
      m_ui->m_groupFeeds->setDisabled(true);

      // Load categories.
      loadCategories(m_serviceRoot->getSubTreeCategories(), m_serviceRoot);
      setWindowTitle(tr("Import feeds"));
      setWindowIcon(qApp->icons()->fromTheme(QSL("document-import")));
      break;
    }

    default:
      break;
  }

  m_ui->m_buttonBox->button(QDialogButtonBox::Ok)->setDisabled(true);
}

void FormStandardImportExport::selectFile() {
  switch (m_model->mode()) {
    case FeedsImportExportModel::Import:
      selectImportFile();
      break;

    case FeedsImportExportModel::Export: {
      selectExportFile();
      break;
    }

    default:
      break;
  }
}

void FormStandardImportExport::onParsingStarted() {
  m_ui->m_lblResult->setStatus(WidgetWithStatus::Progress, tr("Parsing data..."), tr("Parsing data..."));
  m_ui->m_btnSelectFile->setEnabled(false);
  m_ui->m_progressBar->setValue(0);
  m_ui->m_progressBar->setVisible(true);
}

void FormStandardImportExport::onParsingFinished(int count_failed, int count_succeeded, bool parsing_error) {
  Q_UNUSED(count_failed)
  Q_UNUSED(count_succeeded)

  m_ui->m_progressBar->setVisible(false);
  m_ui->m_progressBar->setValue(0);
  m_model->checkAllItems();

  if (!parsing_error) {
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Ok, tr("Feeds were loaded."), tr("Feeds were loaded."));
    m_ui->m_groupFeeds->setEnabled(true);
    m_ui->m_btnSelectFile->setEnabled(true);
    m_ui->m_treeFeeds->setModel(m_model);
    m_ui->m_treeFeeds->expandAll();
  }
  else {
    m_ui->m_groupFeeds->setEnabled(false);
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Error, tr("Error, file is not well-formed. Select another file."),
                                 tr("Error occurred. File is not well-formed. Select another file."));
  }

  m_ui->m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(!parsing_error);
}

void FormStandardImportExport::onParsingProgress(int completed, int total) {
  m_ui->m_progressBar->setMaximum(total);
  m_ui->m_progressBar->setValue(completed);
}

void FormStandardImportExport::selectExportFile() {
  const QString filter_opml20 = tr("OPML 2.0 files (*.opml)");
  const QString filter_txt_url_per_line = tr("TXT files [one URL per line] (*.txt)");

  QString filter;
  QString selected_filter;

  // Add more filters here.
  filter += filter_opml20;
  filter += ";;";
  filter += filter_txt_url_per_line;

  QString selected_file = QFileDialog::getSaveFileName(this, tr("Select file for feeds export"),
                                                       qApp->homeFolderPath(), filter, &selected_filter);

  if (!selected_file.isEmpty()) {
    if (selected_filter == filter_opml20) {
      m_conversionType = OPML20;

      if (!selected_file.endsWith(QL1S(".opml"))) {
        selected_file += QL1S(".opml");
      }
    }
    else if (selected_filter == filter_txt_url_per_line) {
      m_conversionType = TXTUrlPerLine;

      if (!selected_file.endsWith(QL1S(".txt"))) {
        selected_file += QL1S(".txt");
      }
    }

    m_ui->m_lblSelectFile->setStatus(WidgetWithStatus::Ok, QDir::toNativeSeparators(selected_file), tr("File is selected."));
  }

  m_ui->m_buttonBox->button(QDialogButtonBox::Ok)->setDisabled(selected_file.isEmpty());
}

void FormStandardImportExport::selectImportFile() {
  const QString filter_opml20 = tr("OPML 2.0 files (*.opml)");
  const QString filter_txt_url_per_line = tr("TXT files [one URL per line] (*.txt)");

  QString filter;
  QString selected_filter;

  // Add more filters here.
  filter += filter_opml20;
  filter += ";;";
  filter += filter_txt_url_per_line;

  const QString selected_file = QFileDialog::getOpenFileName(this, tr("Select file for feeds import"), qApp->homeFolderPath(),
                                                             filter, &selected_filter);

  if (!selected_file.isEmpty()) {
    if (selected_filter == filter_opml20) {
      m_conversionType = OPML20;
    }
    else if (selected_filter == filter_txt_url_per_line) {
      m_conversionType = TXTUrlPerLine;
    }

    m_ui->m_lblSelectFile->setStatus(WidgetWithStatus::Ok, QDir::toNativeSeparators(selected_file), tr("File is selected."));

    parseImportFile(selected_file);
  }
}

void FormStandardImportExport::parseImportFile(const QString &file_name) {
  QFile input_file(file_name);
  QByteArray input_data;

  if (input_file.open(QIODevice::Text | QIODevice::Unbuffered | QIODevice::ReadOnly)) {
    input_data = input_file.readAll();
    input_file.close();
  }
  else {
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Error, tr("Cannot open source file."), tr("Cannot open source file."));
    return;
  }

  switch (m_conversionType) {
    case OPML20:
      m_model->importAsOPML20(input_data);
      break;

    case TXTUrlPerLine:
      m_model->importAsTxtURLPerLine(input_data);
      break;

      // TODO: V celém kódu nově zavést pořádně všude const, i v lokálních metodových proměnných

      // TODO: Kompletně nahradit všechny ukazatele za QScopedPointer tak,
      // aby se nikde v kodu nevolalo delete či deleteLater().

    default:
      return;
  }
}

void FormStandardImportExport::performAction() {
  switch (m_model->mode()) {
    case FeedsImportExportModel::Import:
      importFeeds();
      break;

    case FeedsImportExportModel::Export:
      exportFeeds();
      break;

    default:
      break;
  }
}

void FormStandardImportExport::exportFeeds() {
  QByteArray result_data;
  bool result_export;

  switch (m_conversionType) {
    case OPML20:
      result_export = m_model->exportToOMPL20(result_data);
      break;

    case TXTUrlPerLine:
      result_export = m_model->exportToTxtURLPerLine(result_data);
      break;

    default:
      break;
  }

  if (result_export) {
    try {
      IOFactory::writeTextFile(m_ui->m_lblSelectFile->label()->text(), result_data);
      m_ui->m_lblResult->setStatus(WidgetWithStatus::Ok, tr("Feeds were exported successfully."), tr("Feeds were exported successfully."));
    }
    catch (IOException &ex) {
      m_ui->m_lblResult->setStatus(WidgetWithStatus::Error, tr("Cannot write into destination file: '%1'."), ex.message());
    }
  }
  else {
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Error, tr("Critical error occurred."), tr("Critical error occurred."));
  }
}

void FormStandardImportExport::importFeeds() {
  QString output_message;
  RootItem *parent = static_cast<RootItem*>(m_ui->m_cmbRootNode->itemData(m_ui->m_cmbRootNode->currentIndex()).value<void*>());

  if (m_serviceRoot->mergeImportExportModel(m_model, parent, output_message)) {
    m_serviceRoot->requestItemExpand(parent->getSubTree(), true);
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Ok, output_message, output_message);
  }
  else {
    m_ui->m_lblResult->setStatus(WidgetWithStatus::Error, output_message, output_message);
  }
}

void FormStandardImportExport::loadCategories(const QList<Category *> categories, RootItem *root_item) {
  m_ui->m_cmbRootNode->addItem(root_item->icon(), root_item->title(), QVariant::fromValue((void*) root_item));

  foreach (Category *category, categories) {
    m_ui->m_cmbRootNode->addItem(category->icon(), category->title(), QVariant::fromValue((void*) category));
  }
}
