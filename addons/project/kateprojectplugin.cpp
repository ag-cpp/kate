/*  This file is part of the Kate project.
 *
 *  SPDX-FileCopyrightText: 2010 Christoph Cullmann <cullmann@kde.org>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kateprojectplugin.h"

#include "qcmakefileapi.h"
#include "kateproject.h"
#include "kateprojectconfigpage.h"
#include "kateprojectpluginview.h"
#include "ktexteditor_utils.h"

#include <kateapp.h>

#include <kcoreaddons_version.h>
#include <ktexteditor/application.h>
#include <ktexteditor/editor.h>
#include <ktexteditor/view.h>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KNetworkMounts>
#include <KSharedConfig>

#include <QApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QString>
#include <QTime>
#include <QTimer>
#include <QThread>

#include <vector>

namespace
{
const QString ProjectFileName = QStringLiteral(".kateproject");
const QString GitFolderName = QStringLiteral(".git");
const QString SubversionFolderName = QStringLiteral(".svn");
const QString MercurialFolderName = QStringLiteral(".hg");
const QString FossilCheckoutFileName = QStringLiteral(".fslckout");
const QString CMakeCacheFileName = QStringLiteral("CMakeCache.txt");

const QString GitConfig = QStringLiteral("git");
const QString SubversionConfig = QStringLiteral("subversion");
const QString MercurialConfig = QStringLiteral("mercurial");
const QString FossilConfig = QStringLiteral("fossil");
const QString CMakeConfig = QStringLiteral("cmake");

const QStringList DefaultConfig = QStringList() << GitConfig << SubversionConfig << MercurialConfig;

const QString CONFIG_ALLOWED_COMMANDS{QStringLiteral("AllowedCommandLines")};
const QString CONFIG_BLOCKED_COMMANDS{QStringLiteral("BlockedCommandLines")};
}

KateProjectPlugin::KateProjectPlugin(QObject *parent, const QVariantList &)
    : KTextEditor::Plugin(parent)
    , m_completion(this)
{
    qRegisterMetaType<KateProjectSharedQStandardItem>("KateProjectSharedQStandardItem");
    qRegisterMetaType<KateProjectSharedQHashStringItem>("KateProjectSharedQHashStringItem");
    qRegisterMetaType<KateProjectSharedProjectIndex>("KateProjectSharedProjectIndex");

    connect(KTextEditor::Editor::instance()->application(), &KTextEditor::Application::documentCreated, this, &KateProjectPlugin::slotDocumentCreated);

    // read configuration prior to cwd project setup below
    readConfig();

    // register all already open documents, later we keep track of all newly created ones
    const auto docs = KTextEditor::Editor::instance()->application()->documents();
    for (auto document : docs) {
        slotDocumentCreated(document);
    }

    // make project plugin variables known to KTextEditor::Editor
    registerVariables();

    // forward to meta-object system friendly version
    connect(this, &KateProjectPlugin::projectCreated, this, &KateProjectPlugin::projectAdded);
    connect(this, &KateProjectPlugin::pluginViewProjectClosing, this, &KateProjectPlugin::projectRemoved);
}

KateProjectPlugin::~KateProjectPlugin()
{
    unregisterVariables();

    for (KateProject *project : qAsConst(m_projects)) {
        delete project;
    }
    m_projects.clear();
}

QObject *KateProjectPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new KateProjectPluginView(this, mainWindow);
}

int KateProjectPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage *KateProjectPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    return new KateProjectConfigPage(parent, this);
}

KateProject *KateProjectPlugin::createProjectForFileName(const QString &fileName)
{
    // check if we already have the needed project opened
    if (auto project = openProjectForDirectory(QFileInfo(fileName).dir())) {
        return project;
    }

    KateProject *project = new KateProject(m_threadPool, this, fileName);
    if (!project->isValid()) {
        delete project;
        return nullptr;
    }

    m_projects.append(project);
    Q_EMIT projectCreated(project);
    return project;
}

KateProject *KateProjectPlugin::openProjectForDirectory(const QDir &dir)
{
    // check for project and load it if found
    const QDir absDir(dir.absolutePath());
    const QString absolutePath = absDir.path();
    const QString projectFileName = absDir.filePath(ProjectFileName);
    for (KateProject *project : qAsConst(m_projects)) {
        if (project->baseDir() == absolutePath || project->fileName() == projectFileName) {
            return project;
        }
    }
    return nullptr;
}

KateProject *KateProjectPlugin::projectForDir(QDir dir, bool userSpecified)
{
    /**
     * Save dir to create a project from directory if nothing works
     */
    const QDir originalDir = dir;

    /**
     * search project file upwards
     * with recursion guard
     * do this first for all level and only after this fails try to invent projects
     * otherwise one e.g. invents projects for .kateproject tree structures with sub .git clones
     */
    QSet<QString> seenDirectories;
    std::vector<QString> directoryStack;
    while (!seenDirectories.contains(dir.absolutePath())) {
        // update guard
        seenDirectories.insert(dir.absolutePath());

        // remember directory for later project creation based on other criteria
        directoryStack.push_back(dir.absolutePath());

        // check for project and load it if found
        if (auto project = openProjectForDirectory(dir)) {
            return project;
        }

        // project file found => done
        if (dir.exists(ProjectFileName)) {
            return createProjectForFileName(dir.filePath(ProjectFileName));
        }

        // else: cd up, if possible or abort
        if (!dir.cdUp()) {
            break;
        }
    }

    /**
     * if we arrive here, we found no .kateproject
     * => we want to invent a project based on e.g. version control system info
     */
    for (const QString &dir : directoryStack) {
        // try to invent project based on version control stuff
        KateProject *project = nullptr;
        if ((project = detectGit(dir)) || (project = detectSubversion(dir)) || (project = detectMercurial(dir)) || (project = detectFossil(dir))
            || (project = detectCMakeBuildTree(dir))) {
            return project;
        }
    }

    /**
     * Version control not found? Load the directory as project
     */
    if (userSpecified) {
        return createProjectForDirectory(originalDir);
    }

    /**
     * Give up
     */
    return nullptr;
}

void KateProjectPlugin::closeProject(KateProject *project)
{
    // collect all documents we have mapped to the projects we want to close
    // we can not delete projects that still have some mapping
    QList<KTextEditor::Document *> projectDocuments;
    for (const auto &it : m_document2Project) {
        if (it.second == project) {
            projectDocuments.append(it.first);
        }
    }

    // if we have some documents open for this project, ask if we want to close, else just do it
    if (!projectDocuments.isEmpty()) {
        QWidget *window = KTextEditor::Editor::instance()->application()->activeMainWindow()->window();
        const QString title = i18n("Confirm project closing: %1", project->name());
        const QString text = i18n("Do you want to close the project %1 and the related %2 open documents?", project->name(), projectDocuments.size());
        if (QMessageBox::Yes != QMessageBox::question(window, title, text, QMessageBox::No | QMessageBox::Yes, QMessageBox::Yes)) {
            return;
        }

        // best effort document closing, some might survive
        KTextEditor::Editor::instance()->application()->closeDocuments(projectDocuments);
    }

    // check: did all documents of the project go away? if not we shall not close it
    if (!projectHasOpenDocuments(project)) {
        Q_EMIT pluginViewProjectClosing(project);
        m_projects.removeOne(project);
        delete project;
    }
}

QList<QObject *> KateProjectPlugin::projectsObjects() const
{
    QList<QObject *> list;
    for (auto &p : m_projects) {
        list.push_back(p);
    }
    return list;
}

bool KateProjectPlugin::projectHasOpenDocuments(KateProject *project) const
{
    for (const auto &it : m_document2Project) {
        if (it.second == project) {
            return true;
        }
    }
    return false;
}

KateProject *KateProjectPlugin::projectForUrl(const QUrl &url)
{
    if (url.isEmpty() || !url.isLocalFile()
        || KNetworkMounts::self()->isOptionEnabledForPath(url.toLocalFile(), KNetworkMounts::MediumSideEffectsOptimizations)) {
        return nullptr;
    }

    return projectForDir(QFileInfo(url.toLocalFile()).absoluteDir());
}

void KateProjectPlugin::slotDocumentCreated(KTextEditor::Document *document)
{
    connect(document, &KTextEditor::Document::documentUrlChanged, this, &KateProjectPlugin::slotDocumentUrlChanged);
    connect(document, &KTextEditor::Document::destroyed, this, &KateProjectPlugin::slotDocumentDestroyed);

    slotDocumentUrlChanged(document);
}

void KateProjectPlugin::slotDocumentDestroyed(QObject *document)
{
    const auto it = m_document2Project.find(static_cast<KTextEditor::Document *>(document));
    if (it == m_document2Project.end()) {
        return;
    }

    it->second->unregisterDocument(static_cast<KTextEditor::Document *>(document));
    m_document2Project.erase(it);
}

void KateProjectPlugin::slotDocumentUrlChanged(KTextEditor::Document *document)
{
    // unregister from old mapping
    slotDocumentDestroyed(document);

    // register for new project, if any
    if (KateProject *project = projectForUrl(document->url())) {
        m_document2Project.emplace(document, project);
        project->registerDocument(document);
    }
}

KateProject *KateProjectPlugin::detectGit(const QDir &dir)
{
    // allow .git as dir and file (file for git worktree stuff, https://git-scm.com/docs/git-worktree)
    if (m_autoGit && dir.exists(GitFolderName)) {
        return createProjectForRepository(QStringLiteral("git"), dir);
    }

    return nullptr;
}

KateProject *KateProjectPlugin::detectSubversion(const QDir &dir)
{
    if (m_autoSubversion && dir.exists(SubversionFolderName) && QFileInfo(dir, SubversionFolderName).isDir()) {
        return createProjectForRepository(QStringLiteral("svn"), dir);
    }

    return nullptr;
}

KateProject *KateProjectPlugin::detectMercurial(const QDir &dir)
{
    if (m_autoMercurial && dir.exists(MercurialFolderName) && QFileInfo(dir, MercurialFolderName).isDir()) {
        return createProjectForRepository(QStringLiteral("hg"), dir);
    }

    return nullptr;
}

KateProject *KateProjectPlugin::detectFossil(const QDir &dir)
{
    if (m_autoFossil && dir.exists(FossilCheckoutFileName) && QFileInfo(dir, FossilCheckoutFileName).isReadable()) {
        return createProjectForRepository(QStringLiteral("fossil"), dir);
    }

    return nullptr;
}

KateProject *KateProjectPlugin::detectCMakeBuildTree(const QDir &dir)
{
    if (m_autoCMake && dir.exists(CMakeCacheFileName) && QFileInfo(dir, CMakeCacheFileName).isFile()) {
        return createProjectForCMakeBuildTree(dir);
    }

    return nullptr;
}


KateProject *KateProjectPlugin::createProjectForRepository(const QString &type, const QDir &dir)
{
    // check if we already have the needed project opened
    if (auto project = openProjectForDirectory(dir)) {
        return project;
    }

    QVariantMap cnf, files;
    files[type] = 1;
    cnf[QStringLiteral("name")] = dir.dirName();
    cnf[QStringLiteral("files")] = (QVariantList() << files);

    KateProject *project = new KateProject(m_threadPool, this, cnf, dir.absolutePath());

    m_projects.append(project);

    Q_EMIT projectCreated(project);
    return project;
}

KateProject *KateProjectPlugin::createProjectForDirectory(const QDir &dir)
{
    // check if we already have the needed project opened
    if (auto project = openProjectForDirectory(dir)) {
        return project;
    }

    QVariantMap cnf, files;
    files[QStringLiteral("directory")] = QStringLiteral("./");
    cnf[QStringLiteral("name")] = dir.dirName();
    cnf[QStringLiteral("files")] = (QVariantList() << files);

    KateProject *project = new KateProject(m_threadPool, this, cnf, dir.absolutePath());

    m_projects.append(project);

    Q_EMIT projectCreated(project);
    return project;
}

KateProject *KateProjectPlugin::createProjectForDirectory(const QDir &dir, const QVariantMap &projectMap)
{
    // check if we already have the needed project opened
    if (auto project = openProjectForDirectory(dir)) {
        return project;
    }

    KateProject *project = new KateProject(m_threadPool, this, projectMap, dir.absolutePath());
    if (!project->isValid()) {
        delete project;
        return nullptr;
    }

    m_projects.append(project);
    Q_EMIT projectCreated(project);
    return project;
}

KateProject *KateProjectPlugin::createProjectForCMakeBuildTree(const QDir &dir)
{
    QCMakeFileApi cmakeFA(dir.absolutePath());
    if (!cmakeFA.haveKateReplyFiles())
    {
        cmakeFA.writeQueryFiles();
        bool success = cmakeFA.runCMake(this);
        qDebug() << "cmake success: " << success;
    }

    if (!cmakeFA.haveKateReplyFiles())
    {
        qDebug() << "generating reply files failed !";
        return nullptr;
    }

    bool success = cmakeFA.readReplyFiles();
    qDebug() << "reply success: " << success;

    QVariantMap cnf;
    QString projectName = QStringLiteral("%1@%2").arg(cmakeFA.getProjectName()).arg(cmakeFA.getBuildDir());
    cnf[QStringLiteral("name")] = projectName;
    cnf[QStringLiteral("directory")] = cmakeFA.getSourceDir();

    QStringList filesList;
    for(const QString& srcFile : cmakeFA.getSourceFiles())
    {
        filesList << srcFile;
    }
    QVariantMap files;
    files[QStringLiteral("list")] = filesList;
    cnf[QStringLiteral("files")] = (QVariantList() << files); //files;

    QVariantMap buildMap;
    buildMap[QStringLiteral("directory")] = cmakeFA.getBuildDir();
    QVariantList targetList;

    QVariantMap tgtMapRerunCMake;
    tgtMapRerunCMake[QStringLiteral("name")] = QStringLiteral("Rerun CMake");
    tgtMapRerunCMake[QStringLiteral("build_cmd")] = QStringLiteral("%1 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -B \"%2\" -S \"%3\"").arg(cmakeFA.getCMakeExecutable())
                                                                              .arg(cmakeFA.getBuildDir())
                                                                              .arg(cmakeFA.getSourceDir());
    targetList << tgtMapRerunCMake;

    QString cmakeGui = cmakeFA.getCMakeGuiExecutable();
    qDebug() << "cmakeGui: " << cmakeGui;
    if (!cmakeGui.isEmpty()) {
      QVariantMap tgtMapRunCMakeGui;
      tgtMapRunCMakeGui[QStringLiteral("name")] = QStringLiteral("Run CMake-Gui");
      tgtMapRunCMakeGui[QStringLiteral("build_cmd")] = QStringLiteral("%1 -B \"%2\"").arg(cmakeGui)
                                                                              .arg(cmakeFA.getBuildDir());
      targetList << tgtMapRunCMakeGui;
    }

    const int numCores = QThread::idealThreadCount();
    const QString emptyConfig(QStringLiteral(" - "));

    for(const QString& config : cmakeFA.getConfigurations()) {
        QVariantMap tgtMap;
        tgtMap[QStringLiteral("name")] = QStringLiteral("[%1] ALL").arg(config.isEmpty() ? emptyConfig : config);
        tgtMap[QStringLiteral("build_cmd")] = QStringLiteral("%1 --build \"%2\" --config \"%3\" --parallel %4")
                                                                              .arg(cmakeFA.getCMakeExecutable())
                                                                              .arg(cmakeFA.getBuildDir())
                                                                              .arg(config)
                                                                              .arg(numCores);
        targetList << tgtMap;
    }

    for(const QCMakeFileApi::TargetDef& tgt : cmakeFA.getTargets()) {
        QVariantMap tgtMap;
        tgtMap[QStringLiteral("name")] = QStringLiteral("[%1] %2").arg(tgt.config.isEmpty() ? emptyConfig : tgt.config).arg(tgt.name);
        tgtMap[QStringLiteral("build_cmd")] = QStringLiteral("%1 --build \"%2\" --config \"%3\" --target \"%4\" --parallel %5")
                                                                              .arg(cmakeFA.getCMakeExecutable())
                                                                              .arg(cmakeFA.getBuildDir())
                                                                              .arg(tgt.config)
                                                                              .arg(tgt.name)
                                                                              .arg(numCores);
        targetList << tgtMap;
    }

    buildMap[QStringLiteral("targets")] = targetList;
    cnf[QStringLiteral("build")] = buildMap;

    QFile::copy(cmakeFA.getBuildDir() + QStringLiteral("/compile_commands.json"), cmakeFA.getSourceDir() + QStringLiteral("/compile_commands.json"));
    qDebug() << "------------- copied b: " << cmakeFA.getBuildDir() << " s: " << cmakeFA.getSourceDir();

    KateProject *project = new KateProject(m_threadPool, this, cnf, dir.absolutePath());

    m_projects.append(project);

    Q_EMIT projectCreated(project);
    return project;

}

void KateProjectPlugin::setAutoRepository(bool onGit, bool onSubversion, bool onMercurial, bool onFossil, bool onCMake)
{
    m_autoGit = onGit;
    m_autoSubversion = onSubversion;
    m_autoMercurial = onMercurial;
    m_autoFossil = onFossil;
    m_autoCMake = onCMake;
    writeConfig();
}

bool KateProjectPlugin::autoGit() const
{
    return m_autoGit;
}

bool KateProjectPlugin::autoSubversion() const
{
    return m_autoSubversion;
}

bool KateProjectPlugin::autoMercurial() const
{
    return m_autoMercurial;
}

bool KateProjectPlugin::autoFossil() const
{
    return m_autoFossil;
}

bool KateProjectPlugin::autoCMake() const
{
    return m_autoCMake;
}

void KateProjectPlugin::setIndex(bool enabled, const QUrl &directory)
{
    m_indexEnabled = enabled;
    m_indexDirectory = directory;
    writeConfig();
}

bool KateProjectPlugin::getIndexEnabled() const
{
    return m_indexEnabled;
}

QUrl KateProjectPlugin::getIndexDirectory() const
{
    return m_indexDirectory;
}

bool KateProjectPlugin::multiProjectCompletion() const
{
    return m_multiProjectCompletion;
}

bool KateProjectPlugin::multiProjectGoto() const
{
    return m_multiProjectGoto;
}

void KateProjectPlugin::setSingleClickAction(ClickAction cb)
{
    m_singleClickAction = cb;
    writeConfig();
}

ClickAction KateProjectPlugin::singleClickAcion()
{
    return m_singleClickAction;
}

void KateProjectPlugin::setDoubleClickAction(ClickAction cb)
{
    m_doubleClickAction = cb;
    writeConfig();
}

ClickAction KateProjectPlugin::doubleClickAcion()
{
    return m_doubleClickAction;
}

void KateProjectPlugin::setMultiProject(bool completion, bool gotoSymbol)
{
    m_multiProjectCompletion = completion;
    m_multiProjectGoto = gotoSymbol;
    writeConfig();
}

void KateProjectPlugin::setRestoreProjectsForSession(bool enabled)
{
    m_restoreProjectsForSession = enabled;
    writeConfig();
}

bool KateProjectPlugin::restoreProjectsForSession() const
{
    return m_restoreProjectsForSession;
}

void KateProjectPlugin::readConfig()
{
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("project"));

    const QStringList autorepository = config.readEntry("autorepository", DefaultConfig);
    m_autoGit = autorepository.contains(GitConfig);
    m_autoSubversion = autorepository.contains(SubversionConfig);
    m_autoMercurial = autorepository.contains(MercurialConfig);
    m_autoFossil = autorepository.contains(FossilConfig);
    m_autoCMake = autorepository.contains(CMakeConfig);

    m_indexEnabled = config.readEntry("index", false);
    m_indexDirectory = config.readEntry("indexDirectory", QUrl());

    m_multiProjectCompletion = config.readEntry("multiProjectCompletion", false);
    m_multiProjectGoto = config.readEntry("multiProjectCompletion", false);

    m_singleClickAction = (ClickAction)config.readEntry("gitStatusSingleClick", (int)ClickAction::NoAction);
    m_doubleClickAction = (ClickAction)config.readEntry("gitStatusDoubleClick", (int)ClickAction::StageUnstage);

    m_restoreProjectsForSession = config.readEntry("restoreProjectsForSessions", false);

    // read allow + block lists as two separate keys, let block always win
    const auto allowed = config.readEntry(CONFIG_ALLOWED_COMMANDS, QStringList());
    const auto blocked = config.readEntry(CONFIG_BLOCKED_COMMANDS, QStringList());
    m_commandLineToAllowedState.clear();
    for (const auto &cmd : allowed) {
        m_commandLineToAllowedState[cmd] = true;
    }
    for (const auto &cmd : blocked) {
        m_commandLineToAllowedState[cmd] = false;
    }

    Q_EMIT configUpdated();
}

void KateProjectPlugin::writeConfig()
{
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("project"));
    QStringList repos;

    if (m_autoGit) {
        repos << GitConfig;
    }

    if (m_autoSubversion) {
        repos << SubversionConfig;
    }

    if (m_autoMercurial) {
        repos << MercurialConfig;
    }

    if (m_autoFossil) {
        repos << FossilConfig;
    }

    if (m_autoCMake) {
        repos << CMakeConfig;
    }

    config.writeEntry("autorepository", repos);

    config.writeEntry("index", m_indexEnabled);
    config.writeEntry("indexDirectory", m_indexDirectory);

    config.writeEntry("multiProjectCompletion", m_multiProjectCompletion);
    config.writeEntry("multiProjectGoto", m_multiProjectGoto);

    config.writeEntry("gitStatusSingleClick", (int)m_singleClickAction);
    config.writeEntry("gitStatusDoubleClick", (int)m_doubleClickAction);

    config.writeEntry("restoreProjectsForSessions", m_restoreProjectsForSession);

    // write allow + block lists as two separate keys
    QStringList allowed, blocked;
    for (const auto &it : m_commandLineToAllowedState) {
        if (it.second) {
            allowed.push_back(it.first);
        } else {
            blocked.push_back(it.first);
        }
    }
    config.writeEntry(CONFIG_ALLOWED_COMMANDS, allowed);
    config.writeEntry(CONFIG_BLOCKED_COMMANDS, blocked);

    Q_EMIT configUpdated();
}

static KateProjectPlugin *findProjectPlugin()
{
    auto plugin = KTextEditor::Editor::instance()->application()->plugin(QStringLiteral("kateprojectplugin"));
    return qobject_cast<KateProjectPlugin *>(plugin);
}

void KateProjectPlugin::registerVariables()
{
    auto editor = KTextEditor::Editor::instance();
    editor->registerVariableMatch(QStringLiteral("Project:Path"),
                                  i18n("Full path to current project excluding the file name."),
                                  [](const QStringView &, KTextEditor::View *view) {
                                      if (!view) {
                                          return QString();
                                      }
                                      auto projectPlugin = findProjectPlugin();
                                      if (!projectPlugin) {
                                          return QString();
                                      }
                                      auto kateProject = findProjectPlugin()->projectForUrl(view->document()->url());
                                      if (!kateProject) {
                                          return QString();
                                      }
                                      return QDir(kateProject->baseDir()).absolutePath();
                                  });

    editor->registerVariableMatch(QStringLiteral("Project:NativePath"),
                                  i18n("Full path to current project excluding the file name, with native path separator (backslash on Windows)."),
                                  [](const QStringView &, KTextEditor::View *view) {
                                      if (!view) {
                                          return QString();
                                      }
                                      auto projectPlugin = findProjectPlugin();
                                      if (!projectPlugin) {
                                          return QString();
                                      }
                                      auto kateProject = findProjectPlugin()->projectForUrl(view->document()->url());
                                      if (!kateProject) {
                                          return QString();
                                      }
                                      return QDir::toNativeSeparators(QDir(kateProject->baseDir()).absolutePath());
                                  });
}
void KateProjectPlugin::unregisterVariables()
{
    auto editor = KTextEditor::Editor::instance();
    editor->unregisterVariable(QStringLiteral("Project:Path"));
    editor->unregisterVariable(QStringLiteral("Project:NativePath"));
}

void KateProjectPlugin::readSessionConfig(const KConfigGroup &config)
{
    // de-serialize all open projects as list of JSON documents if allowed
    if (restoreProjectsForSession()) {
        const auto projectList = config.readEntry("projects", QStringList());
        for (const auto &project : projectList) {
            const QVariantMap sMap = QJsonDocument::fromJson(project.toUtf8()).toVariant().toMap();

            // valid file backed project?
            if (const auto file = sMap[QStringLiteral("file")].toString(); !file.isEmpty() && QFileInfo(file).exists()) {
                createProjectForFileName(file);
                continue;
            }

            // valid path + data project?
            if (const auto path = sMap[QStringLiteral("path")].toString(); !path.isEmpty() && QFileInfo(path).exists()) {
                createProjectForDirectory(QDir(path), sMap[QStringLiteral("data")].toMap());
                continue;
            }

            // we might arrive here if invalid data is store, just ignore that, we just loose session data
        }
    }

    // always load projects from command line or current working directory first time we arrive here
    if (m_initialReadSessionConfigDone) {
        return;
    }
    m_initialReadSessionConfigDone = true;

    /**
     * delayed activation after session restore
     * we do this both for session restoration to not take preference and
     * to be able to signal errors during project loading via message() signals
     */
    KateProject *projectToActivate = nullptr;

    // open directories as projects
    auto args = qApp->arguments();
    args.removeFirst(); // The first argument is the executable name
    for (const QString &arg : qAsConst(args)) {
        QFileInfo info(arg);
        if (info.isDir()) {
            projectToActivate = projectForDir(info.absoluteFilePath(), true);
        }
    }

    /**
     * open project for our current working directory, if this kate has a terminal
     */
    if (!projectToActivate && KateApp::isInsideTerminal()) {
        projectToActivate = projectForDir(QDir::current());
    }

    // if we have some project opened, ensure it is the active one, this happens after session restore
    if (projectToActivate) {
        // delay this to ensure main windows are already there
        QTimer::singleShot(0, projectToActivate, [projectToActivate]() {
            if (auto pluginView = KTextEditor::Editor::instance()->application()->activeMainWindow()->pluginView(QStringLiteral("kateprojectplugin"))) {
                static_cast<KateProjectPluginView *>(pluginView)->openProject(projectToActivate);
            }
        });
    }
}

void KateProjectPlugin::writeSessionConfig(KConfigGroup &config)
{
    // serialize all open projects as list of JSON documents if allowed, always write the list to not leave over old data forever
    QStringList projectList;
    if (restoreProjectsForSession()) {
        for (const auto project : projects()) {
            QVariantMap sMap;

            // for file backed stuff, we just remember the file
            if (project->isFileBacked()) {
                sMap[QStringLiteral("file")] = project->fileName();
            }

            // otherwise we remember the data we generated purely in memory
            else {
                sMap[QStringLiteral("data")] = project->projectMap();
                sMap[QStringLiteral("path")] = project->baseDir();
            }

            // encode as one-lines JSON string
            projectList.push_back(QString::fromUtf8(QJsonDocument::fromVariant(QVariant(sMap)).toJson(QJsonDocument::Compact)));
        }
    }
    config.writeEntry("projects", projectList);
}

void KateProjectPlugin::sendMessage(const QString &text, bool error)
{
    const auto icon = QIcon::fromTheme(QStringLiteral("project-open"));
    Utils::showMessage(text, icon, i18n("Project"), error ? MessageType::Error : MessageType::Info);
}

QString KateProjectPlugin::projectBaseDirForDocument(KTextEditor::Document *doc)
{
    // quick lookup first, then search
    auto project = projectForDocument(doc);
    if (!project) {
        project = projectForUrl(doc->url());
    }
    return project ? project->baseDir() : QString();
}

QVariantMap KateProjectPlugin::projectMapForDocument(KTextEditor::Document *doc)
{
    // quick lookup first, then search
    auto project = projectForDocument(doc);
    if (!project) {
        project = projectForUrl(doc->url());
    }
    return project ? project->projectMap() : QVariantMap();
}

bool KateProjectPlugin::isCommandLineAllowed(const QStringList &cmdline)
{
    // check our allow list
    // if we already have stored some value, perfect, just use that one
    const QString fullCommandLineString = cmdline.join(QStringLiteral(" "));
    if (const auto it = m_commandLineToAllowedState.find(fullCommandLineString); it != m_commandLineToAllowedState.end()) {
        return it->second;
    }

    // ask user if the start should be allowed
    QPointer<QMessageBox> msgBox(new QMessageBox(QApplication::activeWindow()));
    msgBox->setWindowTitle(i18n("Project plugin wants to execute program"));
    msgBox->setTextFormat(Qt::RichText);
    msgBox->setText(
        i18n("The project plugin needs to execute external command for project loading.<br><br>The full command line is:<br><br><b>%1</b><br><br>The choice "
             "can be altered via the config page "
             "of the plugin.",
             fullCommandLineString.toHtmlEscaped()));
    msgBox->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox->setDefaultButton(QMessageBox::Yes);
    const bool allowed = (msgBox->exec() == QMessageBox::Yes);

    // store new configured value
    m_commandLineToAllowedState.emplace(fullCommandLineString, allowed);

    // inform the user if it was forbidden! do this here to just emit this once
    if (!allowed) {
        Q_EMIT showMessage(KTextEditor::Message::Information,
                           i18n("User permanently blocked start of: '%1'.\nUse the config page of the plugin to undo this block.", fullCommandLineString));
    }

    // flush config to not loose that setting
    writeConfig();
    return allowed;
}

#include "moc_kateprojectplugin.cpp"
