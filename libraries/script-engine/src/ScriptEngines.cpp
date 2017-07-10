//
//  Created by Bradley Austin Davis on 2016/01/08
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ScriptEngines.h"

#include <QtCore/QStandardPaths>

#include <QtWidgets/QApplication>

#include <shared/QtHelpers.h>
#include <SettingHandle.h>
#include <UserActivityLogger.h>
#include <PathUtils.h>

#include "ScriptEngine.h"
#include "ScriptEngineLogging.h"

#define __STR2__(x) #x
#define __STR1__(x) __STR2__(x)
#define __LOC__ __FILE__ "(" __STR1__(__LINE__) ") : Warning Msg: "

static const QString DESKTOP_LOCATION = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
static const bool HIFI_SCRIPT_DEBUGGABLES { true };
static const QString SETTINGS_KEY { "RunningScripts" };
static const QUrl DEFAULT_SCRIPTS_LOCATION { "file:///~//defaultScripts.js" };
// Using a QVariantList so this is human-readable in the settings file
static Setting::Handle<QVariantList> runningScriptsHandle(SETTINGS_KEY, { QVariant(DEFAULT_SCRIPTS_LOCATION) });


ScriptsModel& getScriptsModel() {
    static ScriptsModel scriptsModel;
    return scriptsModel;
}

void ScriptEngines::onPrintedMessage(const QString& message, const QString& scriptName) {
    emit printedMessage(message, scriptName);
}

void ScriptEngines::onErrorMessage(const QString& message, const QString& scriptName) {
    emit errorMessage(message, scriptName);
}

void ScriptEngines::onWarningMessage(const QString& message, const QString& scriptName) {
    emit warningMessage(message, scriptName);
}

void ScriptEngines::onInfoMessage(const QString& message, const QString& scriptName) {
    emit infoMessage(message, scriptName);
}

void ScriptEngines::onClearDebugWindow() {
    emit clearDebugWindow();
}

void ScriptEngines::onErrorLoadingScript(const QString& url) {
    emit errorLoadingScript(url);
}

ScriptEngines::ScriptEngines(ScriptEngine::Context context)
    : _context(context),
      _scriptsLocationHandle("scriptsLocation", DESKTOP_LOCATION)
{
    _scriptsModelFilter.setSourceModel(&_scriptsModel);
    _scriptsModelFilter.sort(0, Qt::AscendingOrder);
    _scriptsModelFilter.setDynamicSortFilter(true);
}

QUrl normalizeScriptURL(const QUrl& rawScriptURL) {
    if (rawScriptURL.scheme() == "file") {
        QUrl fullNormal = rawScriptURL;
        QUrl defaultScriptLoc = PathUtils::defaultScriptsLocation();

        // if this url is something "beneath" the default script url, replace the local path with ~
        if (fullNormal.scheme() == defaultScriptLoc.scheme() &&
            fullNormal.host() == defaultScriptLoc.host() &&
            fullNormal.path().startsWith(defaultScriptLoc.path())) {
            fullNormal.setPath("/~/" + fullNormal.path().mid(defaultScriptLoc.path().size()));
        }
        return fullNormal;
    } else if (rawScriptURL.scheme() == "http" || rawScriptURL.scheme() == "https" || rawScriptURL.scheme() == "atp") {
        return rawScriptURL;
    } else {
        // don't accidently support gopher
        return QUrl("");
    }
}

QString expandScriptPath(const QString& rawPath) {
    QStringList splitPath = rawPath.split("/");
    QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
    return defaultScriptsLoc.path() + "/" + splitPath.mid(2).join("/"); // 2 to skip the slashes in /~/
}

QUrl expandScriptUrl(const QUrl& rawScriptURL) {
    QUrl normalizedScriptURL = normalizeScriptURL(rawScriptURL);
    if (normalizedScriptURL.scheme() == "http" ||
        normalizedScriptURL.scheme() == "https" ||
        normalizedScriptURL.scheme() == "atp") {
        return normalizedScriptURL;
    } else if (normalizedScriptURL.scheme() == "file") {
        if (normalizedScriptURL.path().startsWith("/~/")) {
            QUrl url = normalizedScriptURL;
            url.setPath(expandScriptPath(url.path()));

            // stop something like Script.include(["/~/../Desktop/naughty.js"]); from working
            QFileInfo fileInfo(url.toLocalFile());
            url = QUrl::fromLocalFile(fileInfo.canonicalFilePath());

            QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
            if (!defaultScriptsLoc.isParentOf(url)) {
                qCWarning(scriptengine) << "Script.include() ignoring file path" << rawScriptURL
                                        << "-- outside of standard libraries: "
                                        << url.path()
                                        << defaultScriptsLoc.path();
                return rawScriptURL;
            }
            if (rawScriptURL.path().endsWith("/") && !url.path().endsWith("/")) {
                url.setPath(url.path() + "/");
            }
            return url;
        }
        return normalizedScriptURL;
    } else {
        return QUrl("");
    }
}


QObject* scriptsModel();

void ScriptEngines::registerScriptInitializer(ScriptInitializer initializer) {
    _scriptInitializers.push_back(initializer);
}

void ScriptEngines::addScriptEngine(ScriptEngine* engine) {
    if (_isStopped) {
        engine->deleteLater();
    } else {
        QMutexLocker locker(&_allScriptsMutex);
        _allKnownScriptEngines.insert(engine);
    }
}

void ScriptEngines::removeScriptEngine(ScriptEngine* engine) {
    // If we're not already in the middle of stopping all scripts, then we should remove ourselves
    // from the list of running scripts. We don't do this if we're in the process of stopping all scripts
    // because that method removes scripts from its list as it iterates them
    if (!_isStopped) {
        QMutexLocker locker(&_allScriptsMutex);
        _allKnownScriptEngines.remove(engine);
    }
}

void ScriptEngines::shutdownScripting() {
    _isStopped = true;
    QMutexLocker locker(&_allScriptsMutex);
    qCDebug(scriptengine) << "Stopping all scripts.... currently known scripts:" << _allKnownScriptEngines.size();

    QMutableSetIterator<ScriptEngine*> i(_allKnownScriptEngines);
    while (i.hasNext()) {
        ScriptEngine* scriptEngine = i.next();
        QString scriptName = scriptEngine->getFilename();

        // NOTE: typically all script engines are running. But there's at least one known exception to this, the
        // "entities sandbox" which is only used to evaluate entities scripts to test their validity before using
        // them. We don't need to stop scripts that aren't running.
        // TODO: Scripts could be shut down faster if we spread them across a threadpool.
        if (scriptEngine->isRunning()) {
            qCDebug(scriptengine) << "about to shutdown script:" << scriptName;

            // We disconnect any script engine signals from the application because we don't want to do any
            // extra stopScript/loadScript processing that the Application normally does when scripts start
            // and stop. We can safely short circuit this because we know we're in the "quitting" process
            scriptEngine->disconnect(this);

            // Gracefully stop the engine's scripting thread
            scriptEngine->stop();

            // We need to wait for the engine to be done running before we proceed, because we don't
            // want any of the scripts final "scriptEnding()" or pending "update()" methods from accessing
            // any application state after we leave this stopAllScripts() method
            qCDebug(scriptengine) << "waiting on script:" << scriptName;
            scriptEngine->waitTillDoneRunning();
            qCDebug(scriptengine) << "done waiting on script:" << scriptName;

            scriptEngine->deleteLater();

            // Once the script is stopped, we can remove it from our set
            i.remove();
        }
    }
    qCDebug(scriptengine) << "DONE Stopping all scripts....";
}

QVariantList getPublicChildNodes(TreeNodeFolder* parent) {
    QVariantList result;
    QList<TreeNodeBase*> treeNodes = getScriptsModel().getFolderNodes(parent);
    for (int i = 0; i < treeNodes.size(); i++) {
        TreeNodeBase* node = treeNodes.at(i);
        if (node->getType() == TREE_NODE_TYPE_FOLDER) {
            TreeNodeFolder* folder = static_cast<TreeNodeFolder*>(node);
            QVariantMap resultNode;
            resultNode.insert("name", node->getName());
            resultNode.insert("type", "folder");
            resultNode.insert("children", getPublicChildNodes(folder));
            result.append(resultNode);
            continue;
        }
        TreeNodeScript* script = static_cast<TreeNodeScript*>(node);
        if (script->getOrigin() == ScriptOrigin::SCRIPT_ORIGIN_LOCAL) {
            continue;
        }
        QVariantMap resultNode;
        resultNode.insert("name", node->getName());
        resultNode.insert("type", "script");
        resultNode.insert("url", script->getFullPath());
        result.append(resultNode);
    }
    return result;
}

QVariantList ScriptEngines::getPublic() {
    return getPublicChildNodes(NULL);
}

QVariantList ScriptEngines::getLocal() {
    QVariantList result;
    QList<TreeNodeBase*> treeNodes = getScriptsModel().getFolderNodes(NULL);
    for (int i = 0; i < treeNodes.size(); i++) {
        TreeNodeBase* node = treeNodes.at(i);
        if (node->getType() != TREE_NODE_TYPE_SCRIPT) {
            continue;
        }
        TreeNodeScript* script = static_cast<TreeNodeScript*>(node);
        if (script->getOrigin() != ScriptOrigin::SCRIPT_ORIGIN_LOCAL) {
            continue;
        }
        QVariantMap resultNode;
        resultNode.insert("name", node->getName());
        resultNode.insert("path", script->getFullPath());
        result.append(resultNode);
    }
    return result;
}

QVariantList ScriptEngines::getRunning() {
    QVariantList result;
    auto runningScripts = getRunningScripts();
    foreach(const QString& runningScript, runningScripts) {
        QUrl runningScriptURL = QUrl(runningScript);
        if (!runningScriptURL.isValid()) {
            runningScriptURL = QUrl::fromLocalFile(runningScriptURL.toDisplayString(QUrl::FormattingOptions(QUrl::FullyEncoded)));
        }
        QVariantMap resultNode;
        resultNode.insert("name", runningScriptURL.fileName());
        QUrl displayURL = expandScriptUrl(runningScriptURL);
        QString displayURLString;
        if (displayURL.isLocalFile()) {
            displayURLString = displayURL.toLocalFile();
        } else {
            displayURLString = displayURL.toDisplayString(QUrl::FormattingOptions(QUrl::FullyEncoded));
        }
        resultNode.insert("url", displayURLString);
        // The path contains the exact path/URL of the script, which also is used in the stopScript function.
        resultNode.insert("path", normalizeScriptURL(runningScript).toString());
        resultNode.insert("local", runningScriptURL.isLocalFile());
        result.append(resultNode);
    }
    return result;
}

void ScriptEngines::loadDefaultScripts() {
    loadScript(DEFAULT_SCRIPTS_LOCATION);
}

void ScriptEngines::loadOneScript(const QString& scriptFilename) {
    loadScript(scriptFilename);
}

void ScriptEngines::loadScripts() {
    // START BACKWARD COMPATIBILITY CODE
    // The following code makes sure people don't lose all their scripts
    // This should be removed after a reasonable ammount of time went by
    // Load old setting format if present
    bool foundDeprecatedSetting = false;
    Settings settings;
    int size = settings.beginReadArray(SETTINGS_KEY);
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QString string = settings.value("script").toString();
        if (!string.isEmpty()) {
            loadScript(string);
            foundDeprecatedSetting = true;
        }
    }
    settings.endArray();
    if (foundDeprecatedSetting) {
        // Remove old settings found and return
        settings.beginWriteArray(SETTINGS_KEY);
        settings.remove("");
        settings.endArray();
        settings.remove(SETTINGS_KEY + "/size");
        return;
    }
    // END BACKWARD COMPATIBILITY CODE

    // loads all saved scripts
    auto runningScripts = runningScriptsHandle.get();
    for (auto script : runningScripts) {
        auto string = script.toString();
        if (!string.isEmpty()) {
            loadScript(string);
        }
    }
}

void ScriptEngines::saveScripts() {
    // Do not save anything if we are in the process of shutting down
    if (qApp->closingDown()) {
        qWarning() << "Trying to save scripts during shutdown.";
        return;
    }

    // don't save scripts if we started with --scripts, as we would overwrite
    // the scripts that the user expects to be there when launched without the
    // --scripts override.
    if (_defaultScriptsLocationOverridden) {
        return;
    }

    // Saves all currently running user-loaded scripts
    QVariantList list;

    {
        QReadLocker lock(&_scriptEnginesHashLock);
        for (auto it = _scriptEnginesHash.begin(); it != _scriptEnginesHash.end(); ++it) {
            if (it.value() && it.value()->isUserLoaded()) {
                auto normalizedUrl = normalizeScriptURL(it.key());
                list.append(normalizedUrl.toString());
            }
        }
    }

    runningScriptsHandle.set(list);
}

QStringList ScriptEngines::getRunningScripts() {
    QReadLocker lock(&_scriptEnginesHashLock);
    QList<QUrl> urls = _scriptEnginesHash.keys();
    QStringList result;
    for (auto url : urls) {
        result.append(url.toString());
    }
    return result;
}

void ScriptEngines::stopAllScripts(bool restart) {
    QVector<QString> toReload;
    QReadLocker lock(&_scriptEnginesHashLock);

    if (_isReloading) {
        return;
    } else {
        _isReloading = true;
    }

    for (QHash<QUrl, ScriptEngine*>::const_iterator it = _scriptEnginesHash.constBegin();
        it != _scriptEnginesHash.constEnd(); it++) {
        ScriptEngine *scriptEngine = it.value();
        // skip already stopped scripts
        if (scriptEngine->isFinished() || scriptEngine->isStopping()) {
            continue;
        }

        // queue user scripts if restarting
        if (restart && scriptEngine->isUserLoaded()) {
            toReload << it.key().toString();
        }

        // stop all scripts
        qCDebug(scriptengine) << "stopping script..." << it.key();
        scriptEngine->stop();
    }
    // wait for engines to stop (ie: providing time for .scriptEnding cleanup handlers to run) before
    // triggering reload of any Client scripts / Entity scripts
    QTimer::singleShot(1000, this, [=]() {
        for(const auto &scriptName : toReload) {
            auto scriptEngine = getScriptEngine(scriptName);
            if (scriptEngine && !scriptEngine->isFinished()) {
                qCDebug(scriptengine) << "waiting on script:" << scriptName;
                scriptEngine->waitTillDoneRunning();
                qCDebug(scriptengine) << "done waiting on script:" << scriptName;
            }
            qCDebug(scriptengine) << "reloading script..." << scriptName;
            reloadScript(scriptName);
        }
        if (restart) {
            qCDebug(scriptengine) << "stopAllScripts -- emitting scriptsReloading";
            emit scriptsReloading();
        }
        _isReloading = false;
    });
}

bool ScriptEngines::stopScript(const QString& rawScriptURL, bool restart) {
    bool stoppedScript = false;
    {
        QUrl scriptURL = normalizeScriptURL(QUrl(rawScriptURL));
        if (!scriptURL.isValid()) {
            scriptURL = normalizeScriptURL(QUrl::fromLocalFile(rawScriptURL));
        }

        QReadLocker lock(&_scriptEnginesHashLock);
        if (_scriptEnginesHash.contains(scriptURL)) {
            ScriptEngine* scriptEngine = _scriptEnginesHash[scriptURL];
            if (restart) {
                auto scriptCache = DependencyManager::get<ScriptCache>();
                scriptCache->deleteScript(scriptURL);
                connect(scriptEngine, &ScriptEngine::finished, this, [this](QString scriptName, ScriptEngine* engine) {
                    reloadScript(scriptName);
                });
            }
            scriptEngine->stop();
            stoppedScript = true;
            qCDebug(scriptengine) << "stopping script..." << scriptURL;
        }
    }
    return stoppedScript;
}

QString ScriptEngines::getScriptsLocation() const {
    return _scriptsLocationHandle.get();
}

void ScriptEngines::setScriptsLocation(const QString& scriptsLocation) {
    _scriptsLocationHandle.set(scriptsLocation);
    _scriptsModel.updateScriptsLocation(scriptsLocation);
}

void ScriptEngines::reloadAllScripts() {
    qCDebug(scriptengine) << "reloadAllScripts -- clearing caches";
    DependencyManager::get<ScriptCache>()->clearCache();
    qCDebug(scriptengine) << "reloadAllScripts -- stopping all scripts";
    stopAllScripts(true);
}

ScriptEngine* ScriptEngines::loadScript(const QUrl& scriptFilename, bool isUserLoaded, bool loadScriptFromEditor,
                                        bool activateMainWindow, bool reload) {
    if (thread() != QThread::currentThread()) {
        ScriptEngine* result { nullptr };
        BLOCKING_INVOKE_METHOD(this, "loadScript", Q_RETURN_ARG(ScriptEngine*, result),
            Q_ARG(QUrl, scriptFilename),
            Q_ARG(bool, isUserLoaded),
            Q_ARG(bool, loadScriptFromEditor),
            Q_ARG(bool, activateMainWindow),
            Q_ARG(bool, reload));
        return result;
    }
    QUrl scriptUrl;
    if (!scriptFilename.isValid() ||
        (scriptFilename.scheme() != "http" &&
         scriptFilename.scheme() != "https" &&
         scriptFilename.scheme() != "atp" &&
         scriptFilename.scheme() != "file" &&
         scriptFilename.scheme() != "about")) {
        // deal with a "url" like c:/something
        scriptUrl = normalizeScriptURL(QUrl::fromLocalFile(scriptFilename.toString()));
    } else {
        scriptUrl = normalizeScriptURL(scriptFilename);
    }

    auto scriptEngine = getScriptEngine(scriptUrl);
    if (scriptEngine && !scriptEngine->isStopping()) {
        return scriptEngine;
    }

    scriptEngine = new ScriptEngine(_context, NO_SCRIPT, "about:" + scriptFilename.fileName());
    scriptEngine->setUserLoaded(isUserLoaded);
    connect(scriptEngine, &ScriptEngine::doneRunning, this, [scriptEngine] {
        scriptEngine->deleteLater();
    }, Qt::QueuedConnection);


    if (scriptFilename.isEmpty() || !scriptUrl.isValid()) {
        launchScriptEngine(scriptEngine);
    } else {
        // connect to the appropriate signals of this script engine
        connect(scriptEngine, &ScriptEngine::scriptLoaded, this, &ScriptEngines::onScriptEngineLoaded);
        connect(scriptEngine, &ScriptEngine::errorLoadingScript, this, &ScriptEngines::onScriptEngineError);

        // get the script engine object to load the script at the designated script URL
        scriptEngine->loadURL(scriptUrl, reload);
    }

    return scriptEngine;
}

ScriptEngine* ScriptEngines::getScriptEngine(const QUrl& rawScriptURL) {
    ScriptEngine* result = nullptr;
    {
        QReadLocker lock(&_scriptEnginesHashLock);
        const QUrl scriptURL = normalizeScriptURL(rawScriptURL);
        auto it = _scriptEnginesHash.find(scriptURL);
        if (it != _scriptEnginesHash.end()) {
            result = it.value();
        }
    }
    return result;
}

// FIXME - change to new version of ScriptCache loading notification
void ScriptEngines::onScriptEngineLoaded(const QString& rawScriptURL) {
    UserActivityLogger::getInstance().loadedScript(rawScriptURL);
    ScriptEngine* scriptEngine = qobject_cast<ScriptEngine*>(sender());

    launchScriptEngine(scriptEngine);

    {
        QWriteLocker lock(&_scriptEnginesHashLock);
        QUrl url = QUrl(rawScriptURL);
        QUrl normalized = normalizeScriptURL(url);
        _scriptEnginesHash.insertMulti(normalized, scriptEngine);
    }

    // Update settings with new script
    saveScripts();
    emit scriptCountChanged();
}

void ScriptEngines::launchScriptEngine(ScriptEngine* scriptEngine) {
    connect(scriptEngine, &ScriptEngine::finished, this, &ScriptEngines::onScriptFinished, Qt::DirectConnection);
    connect(scriptEngine, &ScriptEngine::loadScript, [&](const QString& scriptName, bool userLoaded) {
        loadScript(scriptName, userLoaded);
    });
    connect(scriptEngine, &ScriptEngine::reloadScript, [&](const QString& scriptName, bool userLoaded) {
        loadScript(scriptName, userLoaded, false, false, true);
    });

    // register our application services and set it off on its own thread
    for (auto initializer : _scriptInitializers) {
        initializer(scriptEngine);
    }

    // FIXME disabling 'shift key' debugging for now.  If you start up the application with
    // the shift key held down, it triggers a deadlock because of script interfaces running
    // on the main thread
    auto const wantDebug = scriptEngine->isDebuggable(); //  || (qApp->queryKeyboardModifiers() & Qt::ShiftModifier);

    if (HIFI_SCRIPT_DEBUGGABLES && wantDebug) {
        scriptEngine->runDebuggable();
    } else {
        scriptEngine->runInThread();
    }
}

void ScriptEngines::onScriptFinished(const QString& rawScriptURL, ScriptEngine* engine) {
    bool removed = false;
    {
        QWriteLocker lock(&_scriptEnginesHashLock);
        const QUrl scriptURL = normalizeScriptURL(QUrl(rawScriptURL));
        for (auto it = _scriptEnginesHash.find(scriptURL); it != _scriptEnginesHash.end(); ++it) {
            if (it.value() == engine) {
                _scriptEnginesHash.erase(it);
                removed = true;
                break;
            }
        }
    }

    if (removed) {
        // Update settings with removed script
        saveScripts();
        emit scriptCountChanged();
    }
}

// FIXME - change to new version of ScriptCache loading notification
void ScriptEngines::onScriptEngineError(const QString& scriptFilename) {
    qCDebug(scriptengine) << "Application::loadScript(), script failed to load...";
    emit scriptLoadError(scriptFilename, "");
}

QString ScriptEngines::getDefaultScriptsLocation() const {
    return PathUtils::defaultScriptsLocation().toString();
}
