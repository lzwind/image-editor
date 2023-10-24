// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "permissionconfig.h"
#include "imageengine.h"

#include <QApplication>
#include <QWindow>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <QPluginLoader>
#include <QDir>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>

#include <DPrintPreviewDialog>

static const QString g_KeyTid = "tid";
static const QString g_KeyOperate = "operate";
static const QString g_KeyFilePath = "filePath";
static const QString g_KeyRemaining = "remainingPrintCount";

// 打印水印插件
static const char *g_WaterMarkEnv = "DEEPIN_WATERMARK";
static const QString g_WaterMarkPluginName = "WaterMarkFilter";
static const QString g_WaterMarkPluginClass = "PrintPreviewSettingsPlugin";

// 打破打印水印间距限制
static const char *g_PrintRowSpacingProp = "_d_print_waterMarkRowSpacing";
static const char *g_PrintColumnSpacingProp = "_d_print_waterMarkColumnSpacing";
static const qreal g_PrintRowSpacingLimit = 10.0;
static const qreal g_PrintColumnSpacingLimit = 2.0;

/**
   @brief 通过dbus接口从任务栏激活窗口
*/
bool activateWindowFromDock(quintptr winId)
{
    bool bRet = false;
    // 优先采用V23接口，低版本DDE还未提供区分V23/V20接口
    QDBusInterface dockDbusInterfaceV23(
        "org.deepin.dde.daemon.Dock1", "/org/deepin/dde/daemon/Dock1", "org.deepin.dde.daemon.Dock1");
    if (dockDbusInterfaceV23.isValid()) {
        QDBusReply<void> reply = dockDbusInterfaceV23.call("ActivateWindow", winId);
        if (!reply.isValid()) {
            qWarning() << qPrintable("Call v23 org.deepin.dde.daemon.Dock1 failed") << reply.error();
        } else {
            return true;
        }
    }

    QDBusInterface dockDbusInterfaceV20(
        "com.deepin.dde.daemon.Dock", "/com/deepin/dde/daemon/Dock", "com.deepin.dde.daemon.Dock");
    if (dockDbusInterfaceV20.isValid() && !bRet) {
        QDBusReply<void> reply = dockDbusInterfaceV20.call("ActivateWindow", winId);
        if (!reply.isValid()) {
            qWarning() << qPrintable("Call v20 com.deepin.dde.daemon.Dock failed") << reply.error();
            bRet = false;
        } else {
            return true;
        }
    }

    return bRet;
}

/**
   @class AuthoriseConfig
   @brief 授权控制类，提供操作授权和水印配置
   @details 授权控制主要包括编辑、拷贝、删除、保存等操作，水印包括阅读水印及打印水印。
    配置信息通过命令行参数获取，当授权控制开启时，进行主要操作将自动发送通知信息。
   @note 此类非线程安全
 */

/**
   @brief 构造函数，构造时即初始化配置
 */
PermissionConfig::PermissionConfig(QObject *parent)
    : QObject(parent)
{
#ifndef DTKWIDGET_CLASS_DWaterMarkHelper
    qWarning() << qPrintable("Current version is not support read watermark");
#endif
}

/**
   @brief 析构函数
 */
PermissionConfig::~PermissionConfig() {}

/**
   @return 返回权限控制单实例
 */
PermissionConfig *PermissionConfig::instance()
{
    static PermissionConfig config;
    return &config;
}

/**
   @return 是否允许进行权限控制，未加载授权配置时返回 false
 */
bool PermissionConfig::isValid() const
{
    return valid;
}

/**
   @return 返回当前是否为权限控制目标图片
 */
bool PermissionConfig::isCurrentIsTargetImage() const
{
    return isValid() && currentImagePath == targetImagePath;
}

/**
   @return 是否允许打印图片，无授权控制时默认返回 true
   @note -1 表示无限制;0 表示无打印次数;1~表示剩余可打印次数
 */
bool PermissionConfig::isPrintable(const QString &fileName) const
{
    if (checkAuthInvalid(fileName)) {
        return true;
    }

    return !!printLimitCount;
}

/**
   @return 是否存在阅读水印
 */
bool PermissionConfig::hasReadWaterMark() const
{
    return authFlags.testFlag(EnableReadWaterMark);
}

/**
   @return 是否存在打印水印
        当环境中存在打印水印插件时，将不再手动设置打印水印，而是通过环境变量设置调整打印水印插件的数据
   @sa detectWaterMarkPluginExists()
 */
bool PermissionConfig::hasPrintWaterMark() const
{
    return !useWaterMarkPlugin && authFlags.testFlag(EnablePrintWaterMark);
}

/**
   @brief 触发打印文件 \a fileName ，若为限权文件，向外发送权限通知信号
 */
void PermissionConfig::triggerPrint(const QString &fileName)
{
    if (checkAuthInvalid(fileName)) {
        return;
    }

    // 减少打印计数
    reduceOnePrintCount();
    QJsonObject data{{g_KeyTid, TidPrint}, {g_KeyOperate, "print"}, {g_KeyFilePath, fileName}, {g_KeyRemaining, printCount()}};

    triggerNotify(data);
}

/**
   @brief 返回当前剩余的打印次数
   @sa `isPrintable`
 */
int PermissionConfig::printCount() const
{
    return printLimitCount;
}

/**
   @brief 返回是否打印无限制
 */
bool PermissionConfig::isUnlimitPrint() const
{
    if (checkAuthInvalid()) {
        return true;
    }
    return -1 == printLimitCount;
}

/**
   @brief 减少一次打印计数并发送打印计数变更信号 `printCountChanged`
 */
void PermissionConfig::reduceOnePrintCount()
{
    if (printLimitCount > 0) {
        printLimitCount--;
        Q_EMIT printCountChanged();
    } else {
        qWarning() << qPrintable("Escape print authorise check!");
    }
}

/**
   @brief 用于操作文件 \a fileName 触发不同类型 \a tid 动作，根据不同动作构造不同的 Json 通知数据，
        并通过信号向外发送。
*/
void PermissionConfig::triggerAction(TidType tid, const QString &fileName)
{
    if (checkAuthInvalid(fileName)) {
        return;
    }

    QString optName;
    switch (tid) {
        case TidOpen:
            if (NotOpen != status) {
                return;
            }
            status = Open;
            optName = "open";
            break;
        case TidClose:
            if (Open != status) {
                return;
            }
            status = Close;
            // Note: 授权文件关闭后(关闭窗口或打开其他图片)，权限控制无效化
            valid = false;

            optName = "close";
            break;
        case TidEdit:
            optName = "edit";
            break;
        case TidCopy:
            optName = "copy";
            break;
        case TidDelete:
            optName = "delete";
            break;
        case TidPrint:
            // 打印操作略有不同，单独处理
            triggerPrint(fileName);
            return;
        default:
            /*
            当前未捕获的操作将不会处理，直接返回
            case TidSwitch:
                optName = "pictureSwitch";
                break;
            case TidSetWallpaper:
                optName = "setWallpaper";
                break;
             */
            return;
    }

    QJsonObject data{{g_KeyTid, tid}, {g_KeyOperate, optName}, {g_KeyFilePath, fileName}};
    triggerNotify(data);
}

/**
   @brief 触发权限操作通知，将向外发送Json数据，通过DBus广播
 */
void PermissionConfig::triggerNotify(const QJsonObject &data)
{
    enum ReportMode { Broadcast = 1, Report = 2, ReportAndBroadcast = Broadcast | Report };
    QJsonObject sendData;
    sendData.insert("policy", QJsonObject{{"reportMode", ReportAndBroadcast}});
    sendData.insert("info", data);

    Q_EMIT authoriseNotify(sendData);
}

/**
   @brief 过滤 \a watched (DPrintPreviewWidget)的 DynamicPropertyChange 事件，
        用于打破 DTK 对打印间距的限制，和设置值一致，仅在设置 breakPrintSpacingLimit 后有效
   @sa installFilterPrintDialog
 */
bool PermissionConfig::eventFilter(QObject *watched, QEvent *event)
{
    if (QEvent::DynamicPropertyChange == event->type() && watched) {
        QDynamicPropertyChangeEvent *propEvent = dynamic_cast<QDynamicPropertyChangeEvent *>(event);
        if (!propEvent) {
            return false;
        }

        auto checkFunc = [&](const char *prop, qreal value, qreal limit){
            if (QByteArray(prop) != propEvent->propertyName()
                    || (value <= limit)) {
                return;
            }

            qreal curValue = watched->property(prop).toDouble();
            if (!qFuzzyCompare(curValue, value)) {
                // 强制设置间距和预期值一致
                watched->setProperty(prop, value);
            }
        };

        // 检查 rowSpacing 和 columnSpacing， DynamicPropertyChange 只是事后通知，无需实际过滤
        checkFunc(g_PrintRowSpacingProp, printRowSpacing, g_PrintRowSpacingLimit);
        checkFunc(g_PrintColumnSpacingProp, printColumnSpacing, g_PrintColumnSpacingLimit);
    }

    return false;
}

/**
   @brief 设置当前处理的文件路径为 \a fileName
 */
void PermissionConfig::setCurrentImagePath(const QString &fileName)
{
    if (!valid) {
        return;
    }

    currentImagePath = fileName;

    // 通知当前展示的图片变更
    Q_EMIT currentImagePathChanged(fileName, bool(currentImagePath == targetImagePath));
}

/**
   @return 返回当前控制的图片路径
 */
QString PermissionConfig::targetImage() const
{
    return targetImagePath;
}

#ifdef DTKWIDGET_CLASS_DWaterMarkHelper

/**
   @return 返回从配置中读取的阅读水印配置，用于图片展示时显示
 */
WaterMarkData PermissionConfig::readWaterMarkData() const
{
    return readWaterMark;
}

/**
   @return 返回从配置中读取的打印水印配置，用于图片打印预览及打印时显示
 */
WaterMarkData PermissionConfig::printWaterMarkData() const
{
    return printWaterMark;
}

#endif  // DTKWIDGET_CLASS_DWaterMarkHelper

/**
   @brief 接收来自外部的调起窗口指令，当 \a pid 和当前进程pid一致时，激活主窗口
*/
void PermissionConfig::activateProcess(qint64 pid)
{
    qInfo() << QString("Receive DBus activate process, current pid: %1, request pid %2").arg(qApp->applicationPid()).arg(pid);

    if (pid == qApp->applicationPid()) {
        QWindowList winList = qApp->topLevelWindows();
        if (winList.isEmpty()) {
            return;
        }

        // 看图默认一个窗口,通过 DBus 接口激活窗口(防止缩小在任务栏)
        auto mainWin = winList.first();
        if (!Q_LIKELY(activateWindowFromDock(mainWin->winId()))) {
            mainWin->requestActivate();
        }
    }
}

/**
   @brief 从命令行参数 \a arguments 中取得授权配置
   @note 命令行参数示例：
    deepin-image-viewer --config=[Base64 code data] /path/to/file
 */
void PermissionConfig::initFromArguments(const QStringList &arguments)
{
    if (valid) {
        return;
    }

    QString configParam;
    QStringList imageList;
    bool ret = parseConfigOption(arguments, configParam, imageList);
    if (ret) {
        // 获取带权限控制的文件路径
        for (const QString &arg : imageList) {
            QFileInfo info(arg);
            QString filePath = info.absoluteFilePath();
            if (ImageEngine::instance()->isImage(filePath) || info.suffix() == "dsps") {
                targetImagePath = filePath;
                break;
            }
        }

        if (targetImagePath.isEmpty()) {
            qWarning() << qPrintable("Authorise config with no target image path.");
            return;
        }
        QByteArray jsonData = QByteArray::fromBase64(configParam.toUtf8());
        qInfo() << QString("Parse authorise config, data: %1").arg(QString(jsonData));

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(jsonData, &error);

        if (!doc.isNull()) {
            QJsonObject root = doc.object();
            initAuthorise(root.value("permission").toObject());
            initReadWaterMark(root.value("readWatermark").toObject());
            initPrintWaterMark(root.value("printWatermark").toObject());

            qInfo() << qPrintable("Current Enable permission") << authFlags;
        } else {
            qWarning()
                << QString("Parse authorise config error at pos: %1, details: %2").arg(error.offset).arg(error.errorString());
        }

        // 存在打印水印设置时，检测是否存在打印水印插件，若存在则通过设置环境变量调用打印插件而不是手动设置
        if (authFlags.testFlag(EnablePrintWaterMark)) {
            detectWaterMarkPluginExists();
        }

        // 只要传入参数、图片即认为有效，无论参数是否正常解析
        valid = true;
        // 首次触发打开图片
        triggerAction(TidOpen, targetImagePath);
    } else {
        qWarning() << qPrintable("Parse authorise config is empty.");
    }

    if (valid) {
        // 传入权限控制参数时，绑定DBus唤醒信号
        QDBusConnection connection = QDBusConnection::sessionBus();
        bool ret = connection.connect(
            "com.wps.cryptfs", "/com/wps/cryptfs", "cryptfs.method.Type", "activateProcess", this, SLOT(activateProcess(qint64)));
        if (!ret) {
            qWarning() << qPrintable("DBus connect activateProcess failed!");
        } else {
            qInfo() << qPrintable("DBus connect activateProcess success!");
        }
    }
}

/**
   @brief 添加 \a dialog 的过滤器，这是用于解除 DTK 中对打印水印间距的限制
        仅在设置 `breakPrintSpacingLimit` 权限后，且打印水印间距系数超过 DTK 限制时有效。
        此函数时预留的，用于预防后期新增需求。
 */
bool PermissionConfig::installFilterPrintDialog(DPrintPreviewDialog *dialog)
{
    if (!breakPrintSpacingLimit) {
        return false;
    }

    // 判断是否需要调整间距，若间距没有超过限制，则无需调整
    if (!(printRowSpacing > g_PrintRowSpacingLimit || printColumnSpacing > g_PrintColumnSpacingLimit)) {
        return false;
    }

    if (dialog) {
        DPrintPreviewWidget *widget = dialog->findChild<DPrintPreviewWidget *>();
        if (widget) {
            widget->installEventFilter(this);
            return true;
        }
    }

    return false;
}

/**
   @return 解析命令行参数并返回设置的权限和水印配置，为设置则返回空
 */
bool PermissionConfig::parseConfigOption(const QStringList &arguments, QString &configParam, QStringList &imageList) const
{
    // 避开以 --config=* 开头的特殊图片
    QStringList specialList;
    QStringList argumentsList = arguments;
    QString prefix("--config=");
    for (auto itr = argumentsList.begin(); itr != argumentsList.end();) {
        if ((*itr).startsWith(prefix) && (*itr).size() > prefix.size()) {
            if (ImageEngine::instance()->isImage((*itr))) {
                specialList.append(*itr);
                itr = argumentsList.erase(itr);
                continue;
            }
        }

        ++itr;
    }

    // 取得 --config 后的数据
    QCommandLineParser parser;
    QCommandLineOption configOpt("config", "Permission config json(base64 encode).", "configvalue");
    parser.addOption(configOpt);

    if (!parser.parse(argumentsList)) {
        qWarning() << QString("Can't parse arguments %1").arg(parser.errorText());
        return false;
    }

    imageList = parser.positionalArguments() + specialList;
    if (parser.isSet(configOpt)) {
        configParam = parser.value(configOpt);
        return true;
    }

    return false;
}

/**
   @brief 从 Json 配置 \a param 中取得授权信息
 */
void PermissionConfig::initAuthorise(const QJsonObject &param)
{
    if (param.isEmpty()) {
        qInfo() << qPrintable("Authorise config not contains authorise data.");
        return;
    }

    // 屏蔽 delete / rename ，默认无此功能
    authFlags.setFlag(EnableEdit, param.value("edit").toBool(false));
    authFlags.setFlag(EnableCopy, param.value("copy").toBool(false));
    authFlags.setFlag(EnableSwitch, param.value("pictureSwitch").toBool(false));
    authFlags.setFlag(EnableWallpaper, param.value("setWallpaper").toBool(false));

    // Internal 内部使用，默认均为false
    ignoreDevicePixelRatio = param.value("ignoreDevicePixelRatio").toBool(false);
    breakPrintSpacingLimit = param.value("breakPrintSpacingLimit").toBool(false);
    if (ignoreDevicePixelRatio) {
        qInfo() << qPrintable("Enable internal property: ignore device pixel ratio.");
    }
    if (breakPrintSpacingLimit) {
        qInfo() << qPrintable("Enable internal property: break print spacing limit.");
    }

    printLimitCount = param.value("printCount").toInt(0);
}

/**
   @brief 从 Json 配置 \a param 中取得阅读水印信息
 */
void PermissionConfig::initReadWaterMark(const QJsonObject &param)
{
    if (param.isEmpty()) {
        qInfo() << qPrintable("Authorise config not contains read watermark data.");
        return;
    }

#ifdef DTKWIDGET_CLASS_DWaterMarkHelper
    readWaterMark.type = WaterMarkType::Text;
    readWaterMark.font.setFamily(param.value("font").toString());
    readWaterMark.font.setPointSize(param.value("fontSize").toInt());

    QString colorName = param.value("color").toString();
    if (!colorName.startsWith('#')) {
        colorName.prepend('#');
    }
    readWaterMark.color.setNamedColor(colorName);
    readWaterMark.opacity = param.value("opacity").toDouble() / 255;
    readWaterMark.layout = param.value("layout").toInt() ? WaterMarkLayout::Tiled : WaterMarkLayout::Center;
    readWaterMark.rotation = param.value("angle").toDouble();
    readWaterMark.lineSpacing = param.value("rowSpacing").toInt();
    readWaterMark.spacing = param.value("columnSpacing").toInt();
    readWaterMark.text = param.value("text").toString();

    qreal deviceRatio = qApp->devicePixelRatio();
    if (ignoreDevicePixelRatio && !qFuzzyCompare(1.0, deviceRatio) && deviceRatio > 0) {
        readWaterMark.font.setPointSizeF(readWaterMark.font.pointSizeF() / deviceRatio);
        readWaterMark.lineSpacing /= deviceRatio;
        readWaterMark.spacing /= deviceRatio;
    }

    authFlags.setFlag(EnableReadWaterMark, true);
#endif  // DTKWIDGET_CLASS_DWaterMarkHelper
}

/**
   @brief 从 Json 配置 \a param 中取得打印水印信息
 */
void PermissionConfig::initPrintWaterMark(const QJsonObject &param)
{
    if (param.isEmpty()) {
        qInfo() << qPrintable("Authorise config not contains print watermark data.");
        return;
    }

#ifdef DTKWIDGET_CLASS_DWaterMarkHelper
    printWaterMark.type = WaterMarkType::Text;
    printWaterMark.font.setFamily(param.value("font").toString());
    printWaterMark.font.setPointSize(param.value("fontSize").toInt());

    QString colorName = param.value("color").toString();
    if (!colorName.startsWith('#')) {
        colorName.prepend('#');
    }
    printWaterMark.color.setNamedColor(colorName);
    printWaterMark.opacity = param.value("opacity").toDouble() / 255;
    printWaterMark.layout = param.value("layout").toInt() ? WaterMarkLayout::Tiled : WaterMarkLayout::Center;
    printWaterMark.rotation = param.value("angle").toDouble();
    printWaterMark.lineSpacing = param.value("rowSpacing").toInt();
    printWaterMark.spacing = param.value("columnSpacing").toInt();
    printWaterMark.text = param.value("text").toString();

    authFlags.setFlag(EnablePrintWaterMark, true);

    // 计算 DTK 打印水印的间距转换系数，限制提升到 10000.0
    if (!printWaterMark.text.isEmpty()) {
        QFontMetrics fm(printWaterMark.font);
        QSize textSize = fm.size(Qt::TextSingleLine, printWaterMark.text);
        if (textSize.height() > 0) {
            printRowSpacing = (qreal(printWaterMark.lineSpacing + textSize.height()) / textSize.height()) - 1.0;
            printRowSpacing = qBound(0.0, printRowSpacing, 10000.0);
        }
        if (textSize.width() > 0) {
            printColumnSpacing = (qreal(printWaterMark.spacing + textSize.width()) / textSize.width()) - 1.0;
            printColumnSpacing = qBound(0.0, printColumnSpacing, 10000.0);
        }

        qInfo() << QString("Print config spacing ratio row: %1 column: %2").arg(printRowSpacing).arg(printColumnSpacing);
    }

#endif  // DTKWIDGET_CLASS_DWaterMarkHelper
}

/**
   @brief 检测当前系统环境中是否存在打印水印插件
        通过路径下是否存在指定的插件文件决定，使用DPKG判断不一定准确，
        文件可能被移除。
   @sa initWaterMarkPluginEnvironment()
 */
void PermissionConfig::detectWaterMarkPluginExists()
{
    if (useWaterMarkPlugin) {
        return;
    }

    // 参考 dprintpreviewdialog.cpp loadPlugin() 加载过程判断插件是否允许加载
    // Note: 打印插件初始化在创建时加载完成，因此此处不尝试加载
#if defined(Q_OS_LINUX)
    QString pluginPath = QLatin1String("/usr/share/deepin/dtk/plugins/printsupport");
#else
    QString pluginPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1String("/printsupport") +
                         QLatin1String("/plugins");
#endif

    if (!QFileInfo(pluginPath).exists()) {
        return;
    }

    QDir pluginDir(pluginPath);
    QStringList fileList = pluginDir.entryList({"*PrintPreviewSettings*.so"}, QDir::Files);

    bool detected = false;
    for (const QString &pluginName : fileList) {
        // 能否正常加载
        QPluginLoader loader(pluginDir.absoluteFilePath(pluginName));
        QString className = loader.metaData().value("className").toString();
        qInfo() << qPrintable("Detect print preview plugin metadata:") << pluginName << loader.metaData();

        if (g_WaterMarkPluginClass == className) {
            detected = true;
            break;
        }
    }

    if (detected) {
        if (initWaterMarkPluginEnvironment()) {
            // 成功初始化打印插件环境变量
            useWaterMarkPlugin = true;
            qInfo() << qPrintable("Using watermark plugin to print preview instead of manual set.");
        } else {
            qWarning() << qPrintable("Detect watermark print preview plugin but init failed!");
        }
    } else {
        qInfo() << qPrintable("Not detect watermark print preview plugin, current plugins:") << fileList;
    }
}

/**
   @brief 初始化打印水印插件环境变量
   @note 打印插件在 loadPlugin() 初始化时读取环境变量，需要注意设置环境变量时间
   @sa detectWaterMarkPluginExists()
 */
bool PermissionConfig::initWaterMarkPluginEnvironment()
{
    QJsonObject envData;
#ifdef DTKWIDGET_CLASS_DWaterMarkHelper
    envData.insert("angle", static_cast<int>(printWaterMark.rotation));
    envData.insert("transparency", static_cast<int>(printWaterMark.opacity * 100));
    QFontMetrics fm(printWaterMark.font);
    QSize textSize = fm.size(Qt::TextSingleLine, printWaterMark.text);
    if (textSize.height() > 0) {
        envData.insert("rowSpacing", qMax(0.0, (qreal(printWaterMark.lineSpacing + textSize.height()) / textSize.height()) - 1.0));
    }
    if (textSize.width() > 0) {
        envData.insert("columnSpacing", qMax(0.0, (qreal(printWaterMark.spacing + textSize.width()) / textSize.width()) - 1.0));
    }

    envData.insert("layout",
                   static_cast<int>(printWaterMark.layout == WaterMarkLayout::Center ? DPrintPreviewWatermarkInfo::Center :
                                                                                       DPrintPreviewWatermarkInfo::Tiled));
    // 仅文本水印
    envData.insert("watermarkType", static_cast<int>(DPrintPreviewWatermarkInfo::TextWatermark));
    envData.insert("textType", static_cast<int>(DPrintPreviewWatermarkInfo::Custom));
    envData.insert("customText", printWaterMark.text);
    envData.insert("textColor", printWaterMark.color.name());
    // 兼容插件传参格式
    QJsonArray fontList;
    fontList.append(printWaterMark.font.family());
    envData.insert("fontList", fontList);
    static const qreal sc_defaultFontSize = 65.0;
    // 字体使用缩放滑块处理 10%~200%, 默认字体大小为65
    envData.insert("size", int(printWaterMark.font.pointSizeF() / sc_defaultFontSize * 100));
#endif

    QJsonDocument doc;
    doc.setObject(envData);
    QByteArray envParam = doc.toJson(QJsonDocument::Compact);
    qInfo() << qPrintable(QString("Set print preview plugin environment %1 PARAM: ").arg(g_WaterMarkEnv)) << envParam;

    if (qputenv(g_WaterMarkEnv, envParam)) {
        DWIDGET_USE_NAMESPACE
        // 打印水印插件可能非默认插件，设置后手动初始化及配置打印
        if (g_WaterMarkPluginName != DPrintPreviewDialog::currentPlugin()) {
            QStringList plugins = DPrintPreviewDialog::availablePlugins();
            if (plugins.contains(g_WaterMarkPluginName)) {
                DPrintPreviewDialog::setCurrentPlugin(g_WaterMarkPluginName);
            } else {
                qWarning() << qPrintable(QString("Print preview plugin not contain %1, all plugins:").arg(g_WaterMarkPluginName))
                           << plugins;
                return false;
            }
        }

    } else {
        qWarning() << qPrintable("Set watermark plugin environment variable failed!");
        return false;
    }

    return true;
}

/**
   @return 返回文件 \a fileName 是否符合权限标识 \a authFlag
*/
bool PermissionConfig::checkAuthFlag(PermissionConfig::Authorise authFlag, const QString &fileName) const
{
    if (checkAuthInvalid(fileName)) {
        return true;
    }

    return authFlags.testFlag(authFlag);
}

/**
   @return 返回文件 \a fileName 是否需要被校验权限
 */
bool PermissionConfig::checkAuthInvalid(const QString &fileName) const
{
    if (!isValid()) {
        return true;
    }
    if (fileName.isEmpty()) {
        return currentImagePath != targetImagePath;
    }

    return fileName != targetImagePath;
}
