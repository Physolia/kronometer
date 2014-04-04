/*
    Copyright (C) 2014 by Elvis Angelaccio <elvis.angelaccio@kdemail.net>

    This file is part of Kronometer.

    Kronometer is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Kronometer is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronometer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mainwindow.h"
  
#include <KApplication>
#include <KLocale>
#include <KAction>
#include <KActionCollection>
#include <KStatusBar>
#include <KConfigDialog>
#include <KFileDialog>
#include <KMessageBox>
#include <KIO/NetAccess>
#include <KSaveFile>

#include <QTableView>
#include <QSplitter>
#include <QClipboard>
#include <QSortFilterProxyModel>
#include <QDomDocument>
#include <QPointer>

#include "qstopwatch.h"
#include "qtimedisplay.h"
#include "lapmodel.h"
#include "settings.h"
#include "widgets/generalsettings.h"
#include "widgets/fontsettings.h"
#include "widgets/colorsettings.h"
#include "widgets/savesettings.h"
#include "widgets/guisettings.h"

namespace
{
    const QString START_KEY = "start";
    const QString PAUSE_KEY = "pause";
    const QString RESET_KEY = "reset";
    const QString LAP_KEY = "lap";
    const QString EXPORT_KEY = "export_laps";

    const QString WINDOW_TITLE = "Kronometer";       /** Default Window title */
    const QString QT_PLACE_HOLDER = "[*]";           /** Qt standard placeholder for setWindowModified() */

    const QString XML_MIMETYPE = "application/xml";
    const QString CSV_MIMETYPE = "text/csv";
    const QString XML_EXTENSION = ".xml";
    const QString CSV_EXTENSION = ".csv";

    // kronometerui.rc states
    const QString INACTIVE_STATE = "inactive";
    const QString RUNNING_STATE = "running";
    const QString PAUSED_STATE = "paused";
    const QString PAUSED_FILE_STATE = "pausedFile";  /** An open file has been paused */

    // XML strings
    const QString STOPWATCH_TAG = "stopwatch";
    const QString LAPS_TAG = "laps";
    const QString LAP_TAG = "lap";
    const QString TIME_TAG = "time";
    const QString ROOT_TAG = "kronometer";
    const QString PERSISTENCE_ATTR = "msec";
    const QString TYPE_ATTR = "type";
    const QString LAP_ID_ATTR = "id";
    const QString REL_TYPE = "relative";
    const QString ABS_TYPE = "absolute";
}

 
MainWindow::MainWindow(QWidget *parent, const QString& file) : KXmlGuiWindow(parent), unsavedTimes(false)
{
    stopwatch = new QStopwatch(this);
    stopwatchDisplay = new QTimeDisplay(this);
    connect(stopwatch, SIGNAL(time(QTime)), stopwatchDisplay, SLOT(time(QTime)));  // bind stopwatch to its display

    setupCentralWidget();
    setupStatusBar();
    setupActions();
    loadSettings();

    setWindowTitle(WINDOW_TITLE + QT_PLACE_HOLDER);

    if (not file.isEmpty()) {
        openFile(file);
    }
}

bool MainWindow::queryClose()
{
    if (stopwatch->isInactive() or not KronometerConfig::askOnExit()) {
        return true;  // exit without ask
    }

    if (stopwatch->isRunning()) {
        stopwatch->pause();
        paused();
    }

    int buttonCode;

    if (fileName.isEmpty()) {
        buttonCode = KMessageBox::warningYesNoCancel(this, i18n("Save times on a new file?"));

        switch (buttonCode) {
        case KMessageBox::Yes:
          saveFileAs();
          return true;  // TODO: return false if saving fails
        case KMessageBox::No:
          return true;
        default: // cancel
          return false;
        }
    }
    else if (unsavedTimes) {
        QFileInfo fileInfo(fileName);
        buttonCode = KMessageBox::warningYesNoCancel(this, i18n("Save times to file %1?", fileInfo.fileName()));

        switch (buttonCode) {
        case KMessageBox::Yes:
          // save document here. If saving fails, return false;
          saveFile();
          return true;
        case KMessageBox::No:
          return true;
        default: // cancel
          return false;
        }
    }

    return true;  // there is an open file, but times are already saved.
}
 
void MainWindow::running()
{
    statusLabel->setText(i18n("Running..."));

    unsavedTimes = true;
    setWindowModified(unsavedTimes);

    stateChanged(RUNNING_STATE);
}

void MainWindow::paused()
{
    startAction->setText(i18n("Re&sume"));
    statusLabel->setText(i18n("Paused"));

    if (not fileName.isEmpty()) {
        stateChanged(PAUSED_FILE_STATE);
    }
    else {
        stateChanged(PAUSED_STATE);
    }

    // the export action can be used only if there are laps (in both the paused states).
    // so, it can't be enabled directly from kronometerui.rc
    if (not lapModel->isEmpty()) {
        exportAction->setEnabled(true);
    }
}

void MainWindow::inactive()
{
    startAction->setText(i18n("&Start"));
    statusLabel->setText(i18n("Inactive"));

    unsavedTimes = false;
    setWindowModified(unsavedTimes);

    stateChanged(INACTIVE_STATE);
}

void MainWindow::showSettings()
{
    if (KConfigDialog::showDialog("settings")) {
        return;
    }

    KConfigDialog* dialog = new KConfigDialog(this, "settings", KronometerConfig::self());

    dialog->showButtonSeparator(true);

    KPageWidgetItem *generalPage = dialog->addPage(new GeneralSettings(this), i18n("General settings"));
    generalPage->setIcon(KIcon(KApplication::windowIcon()));

    KPageWidgetItem *fontPage = dialog->addPage(new FontSettings(this), i18n("Font settings"));
    fontPage->setIcon(KIcon("preferences-desktop-font"));

    KPageWidgetItem *colorPage = dialog->addPage(new ColorSettings(this), i18n("Color settings"));
    colorPage->setIcon(KIcon("fill-color"));

    KPageWidgetItem *guiPage = dialog->addPage(new GuiSettings(this), i18n("Interface settings"));
    guiPage->setIcon(KIcon("preferences-desktop-theme"));

    KPageWidgetItem *savePage = dialog->addPage(new SaveSettings(this), i18n("Save settings"));
    savePage->setIcon(KIcon("document-save"));

    connect(dialog, SIGNAL(settingsChanged(QString)), this, SLOT(writeSettings(QString)));

    dialog->show();
}

void MainWindow::writeSettings(const QString& dialogName)
{
    Q_UNUSED(dialogName);
    KronometerConfig::self()->writeConfig();

    loadSettings();
}

void MainWindow::updateLapDock()
{
    lapView->resizeColumnsToContents();
    lapView->horizontalHeader()->setStretchLastSection(true);
}

void MainWindow::newFile()
{
    MainWindow *window = new MainWindow();
    window->show();
}

void MainWindow::openFile()
{
    QPointer<KFileDialog> dialog = new KFileDialog(KUrl(), QString(), this);
    dialog->setOperationMode(KFileDialog::Opening);
    dialog->setWindowTitle(i18n("Choose a Kronometer save file"));

    QStringList mimeTypes;
    mimeTypes << XML_MIMETYPE;
    dialog->setMimeFilter(mimeTypes);

    if (dialog->exec() == QDialog::Accepted) {
        QString file = dialog->selectedFile();

        if (not file.isEmpty()) {
            MainWindow *window = new MainWindow(nullptr, file);
            window->show();
        }
    }

    delete dialog;
}

void MainWindow::saveFile()
{
    saveFileAs(fileName);
}

void MainWindow::saveFileAs()
{
    QPointer<KFileDialog> dialog = new KFileDialog(KUrl(), QString(), this);
    dialog->setOperationMode(KFileDialog::Saving);
    dialog->setConfirmOverwrite(true);
    dialog->setWindowTitle(i18n("Choose Kronometer save file destination"));

    QStringList mimeTypes;
    mimeTypes << XML_MIMETYPE;
    dialog->setMimeFilter(mimeTypes);

    if (dialog->exec() == QDialog::Accepted) {
        saveFileAs(dialog->selectedFile());
    }

    delete dialog;
}

void MainWindow::exportLapsAs()
{
    QPointer<KFileDialog> dialog = new KFileDialog(KUrl(), QString(), this);
    dialog->setOperationMode(KFileDialog::Saving);
    dialog->setConfirmOverwrite(true);
    dialog->setWindowTitle(i18n("Choose export file destination"));

    QStringList mimeTypes;
    mimeTypes << XML_MIMETYPE << CSV_MIMETYPE;
    dialog->setMimeFilter(mimeTypes, CSV_MIMETYPE);

    if (dialog->exec() == QDialog::Accepted) {
        exportLapsAs(dialog->selectedFile(), dialog->currentMimeFilter());
    }

    delete dialog;
}

void MainWindow::copyToClipboard()
{
    KApplication::clipboard()->setText(stopwatchDisplay->currentTime());
}

void MainWindow::setupCentralWidget()
{
    centralSplitter = new QSplitter(this);

    lapModel = new LapModel(this);
    proxyModel = new QSortFilterProxyModel(this);
    proxyModel->setSourceModel(lapModel);

    lapView = new QTableView(this);
    lapView->setModel(proxyModel);
    lapView->setSelectionBehavior(QAbstractItemView::SelectRows);
    lapView->setGridStyle(Qt::DotLine);
    lapView->verticalHeader()->hide();
    lapView->resizeColumnsToContents();
    lapView->horizontalHeader()->setStretchLastSection(true);
    lapView->setSortingEnabled(true);
    lapView->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Ignored);

    centralSplitter->setOrientation(Qt::Horizontal);
    centralSplitter->setChildrenCollapsible(false);
    centralSplitter->addWidget(stopwatchDisplay);
    centralSplitter->addWidget(lapView);

    setCentralWidget(centralSplitter);
}

void MainWindow::setupStatusBar()
{
    statusBar = new KStatusBar(this);
    statusLabel = new QLabel(this);

    statusLabel->setToolTip(i18n("Current chronometer status"));
    statusBar->addWidget(statusLabel);
    this->setStatusBar(statusBar);
}

void MainWindow::setupActions() 
{
    startAction = new KAction(this);
    pauseAction = new KAction(this);
    resetAction = new KAction(this);
    lapAction = new KAction(this);
    exportAction = new KAction(this);

    startAction->setIcon(KIcon("player-time"));
    startAction->setShortcut(Qt::Key_Space);

    pauseAction->setText(i18n("&Pause"));  // pauseAction/resetAction have fixed text (startAction don't)
    pauseAction->setIcon(KIcon("media-playback-pause"));
    pauseAction->setShortcut(Qt::Key_Space);

    resetAction->setText(i18n("&Reset"));
    resetAction->setIcon(KIcon("edit-clear-history"));
    resetAction->setShortcut(Qt::Key_F5);

    lapAction->setText(i18n("&Lap"));
    lapAction->setIcon(KIcon("chronometer"));
    lapAction->setShortcut(Qt::Key_Return);

    exportAction->setText(i18n("&Export laps as..."));
    exportAction->setIcon(KIcon("document-export"));

    actionCollection()->addAction(START_KEY, startAction);
    actionCollection()->addAction(PAUSE_KEY, pauseAction);
    actionCollection()->addAction(RESET_KEY, resetAction);
    actionCollection()->addAction(LAP_KEY, lapAction);
    actionCollection()->addAction(EXPORT_KEY, exportAction);

    // triggers for QStopwatch "behavioral" slots
    connect(startAction, SIGNAL(triggered(bool)), stopwatch, SLOT(start()));
    connect(pauseAction, SIGNAL(triggered(bool)), stopwatch, SLOT(pause()));
    connect(resetAction, SIGNAL(triggered(bool)), stopwatch, SLOT(reset()));
    connect(lapAction, SIGNAL(triggered(bool)), stopwatch, SLOT(lap()));

    // triggers for LapModel slots
    connect(resetAction, SIGNAL(triggered(bool)), lapModel, SLOT(clear()));
    connect(stopwatch, SIGNAL(lap(QTime)), lapModel, SLOT(lap(QTime)));

    // triggers for MainWindow "gui" slots
    connect(startAction, SIGNAL(triggered(bool)), this, SLOT(running()));
    connect(pauseAction, SIGNAL(triggered(bool)), this, SLOT(paused()));
    connect(resetAction, SIGNAL(triggered(bool)), this, SLOT(inactive()));
    connect(lapAction, SIGNAL(triggered(bool)), this, SLOT(updateLapDock()));

    // File menu triggers
    KStandardAction::quit(this, SLOT(close()), actionCollection());
    KStandardAction::preferences(this, SLOT(showSettings()), actionCollection());
    KStandardAction::openNew(this, SLOT(newFile()), actionCollection());
    KStandardAction::save(this, SLOT(saveFile()), actionCollection());
    KStandardAction::saveAs(this, SLOT(saveFileAs()), actionCollection());
    KStandardAction::open(this, SLOT(openFile()), actionCollection());
    KStandardAction::copy(this, SLOT(copyToClipboard()), actionCollection());
    connect(exportAction, SIGNAL(triggered(bool)), this, SLOT(exportLapsAs()));

    setupGUI(Default, "kronometerui.rc");

    inactive();	// inactive state is the default
}

void MainWindow::loadSettings()
{
    QTimeFormat timeFormat(
        KronometerConfig::showHours(),
        KronometerConfig::showMinutes(),
        KronometerConfig::showSeconds(),
        KronometerConfig::showTenths(),
        KronometerConfig::showHundredths(),
        KronometerConfig::showMilliseconds()
    );

    lapModel->setTimeFormat(timeFormat);
    stopwatchDisplay->setTimeFormat(timeFormat);
    stopwatchDisplay->setHourFont(KronometerConfig::hourFont());
    stopwatchDisplay->setMinFont(KronometerConfig::minFont());
    stopwatchDisplay->setSecFont(KronometerConfig::secFont());
    stopwatchDisplay->setFracFont(KronometerConfig::fracFont());
    stopwatchDisplay->setBackgroundColor(KronometerConfig::backgroundColor());
    stopwatchDisplay->setTextColor(KronometerConfig::textColor());
    stopwatchDisplay->showHeaders(KronometerConfig::showTimeHeaders());
    statusBar->setVisible(KronometerConfig::showStatusBar());

    setupGranularity(KronometerConfig::showTenths(), KronometerConfig::showHundredths(), KronometerConfig::showMilliseconds());
}


void MainWindow::setupGranularity(bool tenths, bool hundredths, bool msec)
{
    if (msec) {
        stopwatch->setGranularity(QStopwatch::MILLISECONDS);
    }
    else if (hundredths) {
        stopwatch->setGranularity(QStopwatch::HUNDREDTHS);
    }
    else if (tenths) {
        stopwatch->setGranularity(QStopwatch::TENTHS);
    }
    else {
        stopwatch->setGranularity(QStopwatch::SECONDS);
    }
}

void MainWindow::saveFileAs(const QString& name)
{
    if (name.isEmpty()) {
        return;
    }

    QString saveName = name;

    if (not saveName.endsWith(XML_EXTENSION)) {
        saveName += XML_EXTENSION;
    }

    KSaveFile saveFile(saveName);
    saveFile.open();

    // OLD: persistence using binary files
    //QDataStream stream(&saveFile);
    //stopwatch->serialize(stream);   // save stopwatch time
    //stream << *lapModel;            // save laps

    // NEW: persistence using XML files
    QTextStream stream(&saveFile);
    createXmlSaveFile(stream);

    saveFile.finalize();
    saveFile.close();

    fileName = saveName;

    unsavedTimes = false;
    setWindowModified(unsavedTimes);
}

void MainWindow::openFile(const QString& name)
{
    QString buffer;

    if (KIO::NetAccess::download(name, buffer, this)) {
        QFile file(buffer);
        file.open(QIODevice::ReadOnly);

        // OLD: persistence using binary files
        //QDataStream stream(&file);
        //stopwatch->deserialize(stream);     // load stopwatch time
        //stream >> *lapModel;                // load laps

        // NEW: persistence using XML files
        QDomDocument doc;
        QString errorMsg;

        if (doc.setContent(&file, &errorMsg)) {
            if (parseXmlSaveFile(doc)) {
                paused();                       // enter in paused state
                fileName = name;

                KIO::NetAccess::removeTempFile(buffer);
                QFileInfo fileInfo(fileName);
                setWindowTitle(WINDOW_TITLE + " - " + fileInfo.fileName() + QT_PLACE_HOLDER);
            }
            else {
                KIO::NetAccess::removeTempFile(buffer);
                close(); // files are opened in a new window, so if the open fails the new window has to be closed.
            }
        }
        else {
            KMessageBox::error(this, "Cannot open file: " + errorMsg);
            KIO::NetAccess::removeTempFile(buffer);
            close();
        }
    }
    else {
        KMessageBox::error(this, KIO::NetAccess::lastErrorString());
    }
}

void MainWindow::createXmlSaveFile(QTextStream& out)
{
    QDomDocument doc;
    QDomProcessingInstruction metaData = doc.createProcessingInstruction("xml", "version='1.0' encoding='UTF-8'");
    QDomComment timestampComment = doc.createComment(timestampMessage());
    QDomElement rootElement = doc.createElement(ROOT_TAG);

    QDomElement stopwatchElement = doc.createElement(STOPWATCH_TAG);
    stopwatch->serialize(stopwatchElement, PERSISTENCE_ATTR);
    QDomElement stopwatchTime = doc.createElement(TIME_TAG);
    stopwatchTime.setAttribute(TYPE_ATTR, ABS_TYPE);
    stopwatchTime.appendChild(doc.createTextNode(stopwatchDisplay->currentTime()));
    stopwatchElement.appendChild(stopwatchTime);

    QDomElement lapsElement = doc.createElement(LAPS_TAG);

    for (int i = 0; i < lapModel->rowCount(QModelIndex()); i++) {
        QDomElement lap = doc.createElement(LAP_TAG);
        lap.setAttribute(LAP_ID_ATTR, i);
        lapModel->lapToXml(lap, PERSISTENCE_ATTR, i);

        QDomElement relTime = doc.createElement(TIME_TAG);
        relTime.setAttribute(TYPE_ATTR, REL_TYPE);
        relTime.appendChild(doc.createTextNode(lapModel->relativeLapTime(i)));

        QDomElement absTime = doc.createElement(TIME_TAG);
        absTime.setAttribute(TYPE_ATTR, ABS_TYPE);
        absTime.appendChild(doc.createTextNode(lapModel->absoluteLapTime(i)));

        lap.appendChild(relTime);
        lap.appendChild(absTime);
        lapsElement.appendChild(lap);
    }

    rootElement.appendChild(stopwatchElement);
    rootElement.appendChild(lapsElement);
    doc.appendChild(metaData);
    doc.appendChild(timestampComment);
    doc.appendChild(rootElement);
    doc.save(out, KronometerConfig::saveFileIndentSize());
}

bool MainWindow::parseXmlSaveFile(const QDomDocument& doc)
{
    QDomElement rootElement = doc.namedItem(ROOT_TAG).toElement();

    if (rootElement.isNull()) {
        KMessageBox::error(this, i18n("Invalid XML file"));
        return false;
    }

    QDomElement stopwatchElement = rootElement.namedItem(STOPWATCH_TAG).toElement();
    QDomElement lapsElement = rootElement.namedItem(LAPS_TAG).toElement();

    if (stopwatchElement.isNull() or lapsElement.isNull()) {
        KMessageBox::error(this, i18n("Incomplete Kronometer save file"));
        return false;
    }

    stopwatch->deserialize(stopwatchElement, PERSISTENCE_ATTR);

    QDomElement lap = lapsElement.firstChildElement(LAP_TAG);

    while (not lap.isNull()) {
        lapModel->lapFromXml(lap, PERSISTENCE_ATTR);
        lap = lap.nextSiblingElement(LAP_TAG);
    }

    return true;
}

void MainWindow::exportLapsAs(const QString& name, const QString& mimetype)
{
    if (name.isEmpty()) {
        return;
    }

    QString exportName = name;

    if (mimetype == XML_MIMETYPE) {
        if (not exportName.endsWith(XML_EXTENSION)) {
            exportName += XML_EXTENSION;
        }

        KSaveFile exportFile(exportName);
        exportFile.open();

        QTextStream stream(&exportFile);
        exportLapsAsXml(stream);

        exportFile.finalize();
        exportFile.close();
    }

    else if (mimetype == CSV_MIMETYPE) {
        if (not exportName.endsWith(CSV_EXTENSION)) {
            exportName += CSV_EXTENSION;
        }

        KSaveFile exportFile(exportName);
        exportFile.open();

        QTextStream stream(&exportFile);
        exportLapsAsCsv(stream);

        exportFile.finalize();
        exportFile.close();
    }
}

void MainWindow::exportLapsAsXml(QTextStream& out)
{
    QDomDocument doc;
    QDomProcessingInstruction metaData = doc.createProcessingInstruction("xml", "version='1.0' encoding='UTF-8'");
    QDomComment timestampComment = doc.createComment(timestampMessage());
    QDomElement rootElement = doc.createElement(ROOT_TAG);

    QDomElement lapsElement = doc.createElement(LAPS_TAG);

    for (int i = 0; i < lapModel->rowCount(QModelIndex()); i++) {
        QDomElement lap = doc.createElement(LAP_TAG);
        lap.setAttribute(LAP_ID_ATTR, i);

        QDomElement relTime = doc.createElement(TIME_TAG);
        relTime.setAttribute(TYPE_ATTR, REL_TYPE);
        relTime.appendChild(doc.createTextNode(lapModel->relativeLapTime(i)));

        QDomElement absTime = doc.createElement(TIME_TAG);
        absTime.setAttribute(TYPE_ATTR, ABS_TYPE);
        absTime.appendChild(doc.createTextNode(lapModel->absoluteLapTime(i)));

        lap.appendChild(relTime);
        lap.appendChild(absTime);
        lapsElement.appendChild(lap);
    }

    rootElement.appendChild(lapsElement);
    doc.appendChild(metaData);
    doc.appendChild(timestampComment);
    doc.appendChild(rootElement);
    doc.save(out, KronometerConfig::exportedXmlIndentSize());
}

void MainWindow::exportLapsAsCsv(QTextStream& out)
{
    out << '#' << timestampMessage() << '\r' << '\n';
    out << '#' << i18n("Lap number,Lap time,Global time") << '\r' << '\n';

    for (int i = 0; i < lapModel->rowCount(QModelIndex()); i++) {
        out << i << ',' << lapModel->relativeLapTime(i) << ',' << lapModel->absoluteLapTime(i) << '\r' << '\n';
    }
}

QString MainWindow::timestampMessage()
{
    QDateTime timestamp = QDateTime::currentDateTime();

    return i18n("Created by Kronometer on %1", timestamp.toString(Qt::DefaultLocaleLongDate));
}

