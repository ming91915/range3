/*********************************************************************
 *  AUTHOR: Tomas Soltys                                             *
 *  FILE:   application.cpp                                          *
 *  GROUP:  Range                                                    *
 *  TYPE:   source file (*.cpp)                                      *
 *  DATE:   28-th June 2013                                          *
 *                                                                   *
 *  DESCRIPTION: Application class definition                        *
 *********************************************************************/

#include <QTimer>
#include <QStyleFactory>
#include <QStyle>
#include <QPalette>
#include <QNetworkProxy>

#include <rmlib.h>

#include "application.h"
#include "locker.h"
#include "logger.h"
#include "progress.h"
#include "main_settings.h"
#include "main_window.h"
#include "material_list.h"
#include "session.h"
#include "file_updater.h"
#include "material_updater.h"
#include "job_manager.h"
#include "rra_session.h"
#include "update_dialog.h"
#include "solver_manager.h"

Application::Application(int &argc, char **argv) :
    QApplication(argc,argv)
{
    // Needed for printf functions family to work correctly.
    setlocale(LC_ALL,"C");

    MaterialList::initialize();
    Locker::initialize();
    Logger::initialize();
    Progress::initialize();

    MainSettings::getInstance().setDirApplicationPath(this->applicationDirPath());

    this->setStyleSheet(QLatin1String("QDialogButtonBox {"
                                      "  button-layout: 0;"
                                      "  dialogbuttonbox-buttons-have-icons: 1;"
                                      "}"));

    QObject::connect(this,&Application::started,this,&Application::onStarted);

    QTimer::singleShot(0, this, SIGNAL(started()));
}

void Application::applyStyle(const QString &styleName)
{
    if (styleName == ApplicationSettings::FusionDark)
    {
        QPalette palette;
    //    palette.setColor(QPalette::Window, QColor(53,53,53));
        palette.setColor(QPalette::Window, QColor(83,83,83));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Base, QColor(15,15,15));
        palette.setColor(QPalette::AlternateBase, QColor(53,53,53));
        palette.setColor(QPalette::ToolTipBase, Qt::white);
        palette.setColor(QPalette::ToolTipText, Qt::black);
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Button, QColor(53,53,53));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, Qt::lightGray);

        palette.setColor(QPalette::Highlight, Qt::lightGray);
        palette.setColor(QPalette::HighlightedText, Qt::black);

        palette.setColor(QPalette::Disabled, QPalette::Text, Qt::darkGray);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, Qt::darkGray);

        QApplication::setPalette(palette);

        QApplication::setStyle(QStyleFactory::create("Fusion"));
    }
    else
    {
        QStyle *style = QStyleFactory::create(styleName);
        QApplication::setPalette(style->standardPalette());
        QApplication::setStyle(style);
    }
}

void Application::onStarted(void)
{
    QStringList filesToLoad;

    // Process command line arguments.
    try
    {
        QList<RArgumentOption> validOptions;
        validOptions.append(RArgumentOption("log-debug",RArgumentOption::Switch,QVariant(),"Switch on debug log level",false,false));
        validOptions.append(RArgumentOption("log-trace",RArgumentOption::Switch,QVariant(),"Switch on trace log level",false,false));

        RArgumentsParser argumentsParser(Application::arguments(),validOptions,true);

        if (argumentsParser.isSet("help"))
        {
            argumentsParser.printHelp();
            this->exit(0);
            return;
        }

        if (argumentsParser.isSet("version"))
        {
            argumentsParser.printVersion();
            this->exit(0);
            return;
        }

        if (argumentsParser.isSet("log-debug"))
        {
            RLogger::getInstance().setLevel(R_LOG_LEVEL_DEBUG);
        }
        if (argumentsParser.isSet("log-trace"))
        {
            RLogger::getInstance().setLevel(R_LOG_LEVEL_TRACE);
        }

        filesToLoad = argumentsParser.getFiles();
    }
    catch (const RError &rError)
    {
        RLogger::error("Failed to parse command line options. %s\n",rError.getMessage().toUtf8().constData());
        this->exit(1);
        return;
    }

    // Connect to aoutToQuit signal
    QObject::connect(this,&Application::aboutToQuit,this,&Application::onAboutToQuit);
    // Connect to style change signal
    QObject::connect(MainSettings::getInstance().getApplicationSettings(),&ApplicationSettings::styleChanged,this,&Application::onStyleChanged);

    // Prepare main window
    MainWindow::getInstance()->show();

    // Set log file
    RLogger::getInstance().setFile(MainSettings::getInstance().getLogDir() + QDir::separator() + "range.log");

    Logger::getInstance().unhalt();

    // Set use system proxy settings
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    // Print basic information
    RLogger::info("Running on: %s\n",QSysInfo::productType().toUtf8().constData());
    RLogger::info("Machine precision (float):  %14g\n", RConstants::findMachineFloatEpsilon());
    RLogger::info("Machine precision (double): %14g\n", RConstants::findMachineDoubleEpsilon());
    RLogger::info("Data directory: \'%s\'\n",MainSettings::getInstancePtr()->getDataDir().toUtf8().constData());
    RLogger::info("Document directory: \'%s\'\n",MainSettings::getInstancePtr()->getDocDir().toUtf8().constData());
    RLogger::info("Log directory: \'%s\'\n",MainSettings::getInstancePtr()->getLogDir().toUtf8().constData());
    RLogger::info("Temporary directory: \'%s\'\n",MainSettings::getInstancePtr()->getTmpDir().toUtf8().constData());
    RLogger::info("Material directory: \'%s\'\n",MainSettings::getInstancePtr()->getMaterialsDir().toUtf8().constData());
    RLogger::info("Session directory: \'%s\'\n",MainSettings::getInstancePtr()->getSessionDir().toUtf8().constData());
    RLogger::info("Solver path: \'%s\'\n",MainSettings::getInstancePtr()->getApplicationSettings()->getSolverPath().toUtf8().constData());

    // Read material database
    MaterialList::getInstance().setStorePath(MainSettings::getInstance().getMaterialsDir() + QDir::separator());

    // Check current version.
    RLogger::info("Checking current against last used software version.\n");
    RLogger::indent();
    RLogger::info("Last used version: %s\n",MainSettings::getInstance().getStoredVersion().toString().toUtf8().constData());
    RLogger::info("Currently used version: %s\n",RVendor::version.toString().toUtf8().constData());
    RLogger::unindent();

    if (RVendor::version > MainSettings::getInstance().getStoredVersion())
    {
        // Newer version is being executed.

        // Perform material database update.
        RLogger::info("Preparing material database update.\n");
        RLogger::indent();

        MaterialUpdater *pMaterialUpdater = new MaterialUpdater;

        QDir matSrcDir(QDir::cleanPath(QDir(this->applicationDirPath()).filePath("../materials")));

        RLogger::info("Source directory: \'%s\'\n",matSrcDir.absolutePath().toUtf8().constData());

        if (matSrcDir.exists())
        {
            QStringList files = matSrcDir.entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
            foreach (QString file, files)
            {
                RLogger::info("Adding material: \'%s\'\n",matSrcDir.filePath(file).toUtf8().constData());
                pMaterialUpdater->addMaterial(matSrcDir.filePath(file));
            }
        }
        RLogger::unindent();

        // Perform data files update.
        RLogger::info("Preparing data files update.\n");
        RLogger::indent();

        FileUpdater *pFileUpdater = new FileUpdater;

        QDir dataSrcDir(QDir::cleanPath(QDir(this->applicationDirPath()).filePath("../data")));
        QDir dataDstDir(MainSettings::getInstance().getDataDir());

        RLogger::info("Source directory: \'%s\'\n",dataSrcDir.absolutePath().toUtf8().constData());
        RLogger::info("Destination directory: \'%s\'\n",dataDstDir.absolutePath().toUtf8().constData());

        if (dataSrcDir.exists())
        {
            QStringList files = dataSrcDir.entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
            foreach (QString file, files)
            {
                RLogger::info("Adding file: \'%s\' -> \'%s\'\n",dataSrcDir.filePath(file).toUtf8().constData(),dataDstDir.filePath(file).toUtf8().constData());
                pFileUpdater->addFile(dataSrcDir.filePath(file),dataDstDir.filePath(file));
            }
        }
        RLogger::unindent();

        JobManager::getInstance().submit(pMaterialUpdater);
        JobManager::getInstance().submit(pFileUpdater);
    }

    // Start RRA Session
    RRASession::getInstance().start();

    QObject::connect(&RRASession::getInstance(),&RRASession::availableSoftware,this,&Application::onAvailableSoftware);
    QObject::connect(&RRASession::getInstance(),&RRASession::signedIn,this,&Application::onSignedIn);
    QObject::connect(&RRASession::getInstance(),&RRASession::signedOut,this,&Application::onSignedOut);

    if (!filesToLoad.isEmpty())
    {
        try
        {
            Session::getInstance().readModels(filesToLoad);
        }
        catch (const RError &rError)
        {
            RLogger::warning("Failed to read model file(s). ERROR: %s\n",rError.getMessage().toUtf8().constData());
        }
    }
    else
    {
        QString sessionFileName = MainSettings::getInstance().getSessionFileName();
        if (!sessionFileName.isEmpty() && RFileManager::fileExists(sessionFileName))
        {
            try
            {
                Session::getInstance().read(sessionFileName);
            }
            catch (const RError &rError)
            {
                RLogger::warning("Failed to read the session file \'%s\'. ERROR: %s\n",sessionFileName.toUtf8().constData(),rError.getMessage().toUtf8().constData());
            }
        }
    }
}

void Application::onAboutToQuit(void)
{
    QString sessionFileName = Session::getInstance().getFileName();
    if (sessionFileName.isEmpty())
    {
        sessionFileName = Session::getInstance().getDefaultFileName();
    }
    try
    {
        Session::getInstance().write(sessionFileName);
    }
    catch (const RError &error)
    {
        RLogger::error("Failed to write the session file \'%s\'. ERROR: %s\n",sessionFileName.toUtf8().constData(),error.getMessage().toUtf8().constData());
    }

    // Stop solver manager server.
    RLogger::info("Stoping solver task server\n");
    SolverManager::getInstance().stopServer();

    // Stop RRA Session
    RLogger::info("Stoping RRA Session\n");
    RRASession::getInstance().stop();

    // Stop logger
    RLogger::info("Stoping logger\n");
    Logger::halt();

    // Delete main window
    delete MainWindow::getInstance();
}

void Application::onStyleChanged(const QString &styleName)
{
    this->applyStyle(styleName);
}

void Application::onAvailableSoftware(const RVersion &version, const QString &link)
{
    if (RVendor::version < version)
    {
        UpdateDialog updateDialog(version,link,MainWindow::getInstance());
        updateDialog.exec();
    }
}

void Application::onSignedIn(void)
{

}

void Application::onSignedOut(void)
{

}
