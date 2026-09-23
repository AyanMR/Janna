#include "ui/OpggWindow.h"

#include "services/AssistController.h"
#include "services/ChampionRepository.h"
#include "ui/ChampionPicker.h"
#include "ui/LineIconButton.h"

#include <QCloseEvent>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHash>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QToolButton>
#include <QSvgRenderer>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace Janna {
namespace {

QString pct(const double value)
{
    return value <= 0.0 ? QStringLiteral("-") : QString::number(value, 'f', 1) + "%";
}

QPixmap grayIcon(QPixmap pixmap)
{
    if (pixmap.isNull()) return pixmap;
    QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(line[x]);
            const int value = qGray(color.rgb());
            // `rgb()` normalizes alpha to 255. Preserve the source alpha so
            // transparent rune corners stay transparent after desaturation,
            // and opaque pixels do not acquire a dark translucent halo.
            line[x] = qRgba(value, value, value, color.alpha());
        }
    }
    return QPixmap::fromImage(image);
}

QPixmap circularIcon(const QPixmap &source, const int size)
{
    if (source.isNull() || size <= 0) return {};
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);

    // Apply the circle as an alpha mask after scaling.  Clipping only the
    // painter path can leave premultiplied black pixels on the antialiased
    // edge when the source icon has an opaque square background.  Destination
    // alpha keeps every corner fully transparent and works for both selected
    // and grayscale rune images.
    painter.end();
    QImage mask(size, size, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter maskPainter(&mask);
    maskPainter.setRenderHint(QPainter::Antialiasing, true);
    maskPainter.setBrush(Qt::white);
    maskPainter.setPen(Qt::NoPen);
    maskPainter.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
    maskPainter.end();
    QPainter alphaPainter(&image);
    alphaPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    alphaPainter.drawImage(0, 0, mask);
    alphaPainter.end();
    // Some Windows paint engines preserve RGB bytes in fully transparent
    // pixels when converting a premultiplied image to QPixmap. Zero those
    // bytes explicitly so a square source can never leave a dark corner
    // behind when the icon is composited on a light panel.
    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0) line[x] = qRgba(0, 0, 0, 0);
        }
    }
    return QPixmap::fromImage(image);
}

QString normalizedRuneImageUrl(QString value)
{
    value = value.trimmed();
    // HTML-rendered OP.GG payloads occasionally leave query separators
    // escaped even though the URL is carried as an image attribute.
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
    if (value.startsWith(QStringLiteral("//"))) return QStringLiteral("https:") + value;
    if (value.startsWith(QStringLiteral("/"))) {
        // OP.GG occasionally emits a root-relative asset path in older
        // payloads.  Keep the fallback on the public static asset host.
        return QStringLiteral("https://opgg-static.akamaized.net") + value;
    }
    return value;
}

QString fallbackRuneImageUrl(const int id, QString version)
{
    if (id <= 0) return {};
    version = version.trimmed();
    static const QRegularExpression patchPattern(QStringLiteral("^\\d{1,2}\\.\\d{1,2}(?:\\.\\d+)?$"));
    if (!patchPattern.match(version).hasMatch()) return {};
    if (version.count(QLatin1Char('.')) == 1) version += QStringLiteral(".1");
    const QString assetDirectory = id >= 5000 && id < 5100
        ? QStringLiteral("perkShard") : QStringLiteral("perk");
    return QStringLiteral("https://opgg-static.akamaized.net/meta/images/lol/%1/%2/%3.png")
        .arg(version, assetDirectory, QString::number(id));
}

QColor themedIconColor()
{
    if (qApp) {
        const QColor configured = qApp->property("jannaIcon").value<QColor>();
        if (configured.isValid()) return configured;
        return qApp->palette().color(QPalette::WindowText);
    }
    return QColor(QStringLiteral("#68746D"));
}

QIcon backIcon()
{
    constexpr int size = 24;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(QStringLiteral(":/janna/opgg/icons/opgg-back.svg"));
    if (renderer.isValid()) {
        {
            QPainter painter(&pixmap);
            renderer.render(&painter);
        }
        // The resource supplies the geometry while the application palette
        // supplies the contrast. This keeps the arrow visible in both themes.
        QPainter tint(&pixmap);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(), themedIconColor());
        return QIcon(pixmap);
    }
    // The fallback is deliberately painted instead of using a text glyph:
    // font fallback differs across Windows installations and was the source
    // of the previously blank-looking navigation button.
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(themedIconColor(), 2.0, Qt::SolidLine,
             Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(QPointF(15.5, 4.5), QPointF(7.5, 12.0));
    painter.drawLine(QPointF(7.5, 12.0), QPointF(15.5, 19.5));
    painter.drawLine(QPointF(8.0, 12.0), QPointF(18.0, 12.0));
    return QIcon(pixmap);
}

QString championName(const ChampionRepository &repository, const int id, const QString &fallback = {})
{
    const QString local = repository.championNameFor(id);
    if (!local.isEmpty()) return local;
    if (!fallback.trimmed().isEmpty()) return fallback.trimmed();
    return id > 0 ? QStringLiteral("未知英雄") : QStringLiteral("当前英雄");
}

QString championTypeText(const ChampionRepository &repository, const int id)
{
    const QStringList types = repository.championTypeLabelsFor(id);
    return types.isEmpty() ? QStringLiteral("定位：未同步")
                           : QStringLiteral("定位：") + types.join(QStringLiteral(" / "));
}

QString opggTypeText(const ChampionRepository &repository, const int id, const QStringList &roles)
{
    return roles.isEmpty() ? championTypeText(repository, id)
                           : QStringLiteral("定位：") + roles.join(QStringLiteral(" / "));
}

QString championKey(QString value)
{
    value = value.trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (value == QStringLiteral("nunuwillump")) value = QStringLiteral("nunu");
    return value;
}

int championIdForRank(const ChampionRepository &repository, const OpggChampionRank &entry)
{
    return repository.resolveChampionId(entry.championId,
                                        !entry.championKey.trimmed().isEmpty() ? entry.championKey : entry.championName);
}

QString displayTier(const QString &raw)
{
    const QString value = raw.trimmed();
    if (value.isEmpty()) return QStringLiteral("-");
    const QString compact = value.toUpper().remove(' ').remove('_').remove('-');
    if (compact == QStringLiteral("0") || compact == QStringLiteral("OP") || compact == QStringLiteral("OPGOD")
        || compact == QStringLiteral("OPGG")) return QStringLiteral("OP");
    if (compact == QStringLiteral("TIER0")) return QStringLiteral("OP");
    if (compact.startsWith(QStringLiteral("TIER"))) {
        const QString number = compact.mid(4);
        if (number.toInt() > 0) return QStringLiteral("T") + number;
    }
    if (compact.toInt() > 0) return QStringLiteral("T") + compact;
    return value.toUpper();
}

QString championSlug(const ChampionRepository &repository, const int id)
{
    QString name = repository.championKeyFor(id);
    QString portraitPath;
    for (const Champion &champion : repository.champions()) {
        if (champion.id != id) continue;
        if (name.isEmpty()) name = champion.key;
        portraitPath = champion.squarePortraitPath;
        break;
    }
    if (name.trimmed().isEmpty()) return {};
    const QRegularExpression pathPattern(QStringLiteral("/Characters/([^/]+)/"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch pathMatch = pathPattern.match(portraitPath);
    if (pathMatch.hasMatch()) name = pathMatch.captured(1);
    QString slug = name.trimmed().toLower();
    slug.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (slug == QStringLiteral("nunuwillump")) slug = QStringLiteral("nunu");
    return slug;
}

QString runeNames(const ChampionRepository &repository, const QList<int> &ids)
{
    QStringList names;
    for (const int id : ids) {
        const QString name = repository.runeNameFor(id).trimmed();
        names.append(name.isEmpty() ? QString::number(id) : name);
    }
    return names.isEmpty() ? QStringLiteral("无") : names.join(QStringLiteral("、"));
}

QString spellNames(const ChampionRepository &repository, const QList<int> &ids)
{
    QStringList names;
    for (const int id : ids) {
        const QString name = repository.summonerSpellNameFor(id).trimmed();
        names.append(name.isEmpty() ? QString::number(id) : name);
    }
    return names.isEmpty() ? QStringLiteral("未提供") : names.join(QStringLiteral(" / "));
}

QString itemNames(const ChampionRepository &repository, const QList<int> &ids)
{
    QStringList names;
    for (const int id : ids) {
        const QString name = repository.itemNameFor(id).trimmed();
        names.append(name.isEmpty() ? QString::number(id) : name);
    }
    return names.isEmpty() ? QStringLiteral("未提供") : names.join(QStringLiteral("、"));
}

bool statShardId(const int id)
{
    return id >= 5000 && id < 5100;
}

QString localizedRuneName(int id);

QString runeDisplayName(const ChampionRepository &repository, const int id)
{
    const QString localizedName = localizedRuneName(id);
    if (!localizedName.isEmpty()) return localizedName;
    QString name = repository.runeNameFor(id).trimmed();
    // CommunityDragon is the fallback catalog when the LCU is not running;
    // avoid exposing an opaque numeric fallback in the OPGG detail view.
    if (name.isEmpty() || name == QStringLiteral("符文 ") + QString::number(id)
        || name == QStringLiteral("Rune ") + QString::number(id)) {
        static const QHash<int, QString> localized = {
            {5001, QStringLiteral("生命值")}, {5002, QStringLiteral("护甲")},
            {5003, QStringLiteral("魔抗")}, {5005, QStringLiteral("攻击速度")},
            {5007, QStringLiteral("技能急速")}, {5008, QStringLiteral("自适应之力")},
            {5010, QStringLiteral("韧性与减速抵抗")}, {5011, QStringLiteral("生命值")},
            {5013, QStringLiteral("韧性与减速抵抗")}, {5016, QStringLiteral("生命值成长")},
            {5021, QStringLiteral("生命值成长")}
        };
        name = localized.value(id);
    }
    return name.isEmpty() ? QStringLiteral("推荐符文") : name;
}

QString spellDisplayName(const ChampionRepository &repository, const int id)
{
    QString name = repository.summonerSpellNameFor(id).trimmed();
    if (name.isEmpty() || name == QStringLiteral("召唤师技能 ") + QString::number(id)) {
        static const QHash<int, QString> localized = {
            {1, QStringLiteral("净化")}, {3, QStringLiteral("虚弱")}, {4, QStringLiteral("闪现")},
            {6, QStringLiteral("幽灵疾步")}, {7, QStringLiteral("治疗")}, {11, QStringLiteral("惩戒")},
            {12, QStringLiteral("传送")}, {13, QStringLiteral("清晰术")}, {14, QStringLiteral("点燃")},
            {21, QStringLiteral("屏障")}, {32, QStringLiteral("雪球")}
        };
        name = localized.value(id);
    }
    return name.isEmpty() ? QStringLiteral("召唤师技能") : name;
}

QString itemDisplayName(const ChampionRepository &repository, const int id)
{
    const QString name = repository.itemNameFor(id).trimmed();
    return name.isEmpty() || name == QStringLiteral("装备 ") + QString::number(id)
        ? QStringLiteral("推荐装备") : name;
}

void clearContentLayout(QWidget *content)
{
    if (!content || !content->layout()) return;
    QLayout *layout = content->layout();
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) delete widget;
        delete item;
    }
}

QWidget *assetChip(QWidget *parent, const QString &name, const QPixmap &pixmap,
                   const int iconSize, const QString &tooltip = {})
{
    auto *chip = new QWidget(parent);
    chip->setObjectName(QStringLiteral("opggAssetChip"));
    chip->setMinimumWidth(84);
    auto *layout = new QVBoxLayout(chip);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    auto *icon = new QLabel(chip);
    icon->setObjectName(QStringLiteral("opggAssetIcon"));
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(iconSize, iconSize);
    if (!pixmap.isNull()) {
        icon->setPixmap(pixmap.scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        icon->setText(QStringLiteral("·"));
    }
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    auto *label = new QLabel(name, chip);
    label->setObjectName(QStringLiteral("opggAssetName"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMinimumHeight(24);
    layout->addWidget(label);
    if (!tooltip.isEmpty()) chip->setToolTip(tooltip);
    return chip;
}

QWidget *runeOptionChip(QWidget *parent, const QString &name, const QPixmap &pixmap,
                        const int iconSize, const bool selected, const bool enabled = true)
{
    const QPixmap runePixmap = circularIcon(selected ? pixmap : grayIcon(pixmap), iconSize);
    auto *chip = assetChip(parent, name, runePixmap, iconSize, name);
    chip->setObjectName(QStringLiteral("opggRuneOption"));
    chip->setProperty("runeSelected", selected);
    chip->setEnabled(enabled);
    if (auto *icon = chip->findChild<QLabel *>(QStringLiteral("opggAssetIcon"))) {
        // Clip the widget as well as the pixmap so the stylesheet background
        // cannot paint square corners behind a circular rune image.
        icon->setMask(QRegion(QRect(0, 0, iconSize, iconSize), QRegion::Ellipse));
        icon->setAttribute(Qt::WA_TranslucentBackground);
        icon->setAttribute(Qt::WA_NoSystemBackground);
        icon->setAutoFillBackground(false);
        icon->setScaledContents(false);
        icon->setStyleSheet(QStringLiteral("background: transparent; border: none; border-radius: 0px;"));
    }
    if (!enabled) chip->setToolTip(QStringLiteral("副系基石不可选"));
    return chip;
}

QWidget *sectionLabel(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("opggAssetSection"));
    return label;
}

QString runeStyleName(const int styleId)
{
    switch (styleId) {
    case 8000: return QStringLiteral("精密");
    case 8100: return QStringLiteral("主宰");
    case 8200: return QStringLiteral("巫术");
    case 8300: return QStringLiteral("启迪");
    case 8400: return QStringLiteral("坚决");
    default: return styleId > 0 ? QString::number(styleId) : QStringLiteral("未提供");
    }
}

struct RuneRowDefinition {
    QString label;
    QList<int> ids;
};

QList<RuneRowDefinition> runeRowsForStyle(const int styleId)
{
    switch (styleId) {
    case 8000: // Precision
        return {{QStringLiteral("基石"), {8005, 8008, 8021, 8010}},
                {QStringLiteral("第一行"), {9101, 9111, 8009}},
                {QStringLiteral("第二行"), {9104, 9105, 9103}},
                {QStringLiteral("第三行"), {8014, 8017, 8299}}};
    case 8100: // Domination
        return {{QStringLiteral("基石"), {8112, 8124, 8128, 9923}},
                {QStringLiteral("第一行"), {8126, 8139, 8143}},
                {QStringLiteral("第二行"), {8136, 8137, 8138}},
                {QStringLiteral("第三行"), {8105, 8106, 8134, 8135}}};
    case 8200: // Sorcery
        return {{QStringLiteral("基石"), {8214, 8229, 8230}},
                {QStringLiteral("第一行"), {8224, 8226, 8275}},
                {QStringLiteral("第二行"), {8210, 8234, 8233}},
                {QStringLiteral("第三行"), {8237, 8236, 8232}}};
    case 8300: // Inspiration
        return {{QStringLiteral("基石"), {8351, 8360, 8369}},
                {QStringLiteral("第一行"), {8304, 8306, 8313}},
                {QStringLiteral("第二行"), {8345, 8347, 8316}},
                {QStringLiteral("第三行"), {8321, 8339, 8352}}};
    case 8400: // Resolve
        return {{QStringLiteral("基石"), {8437, 8439, 8465}},
                {QStringLiteral("第一行"), {8446, 8463, 8401}},
                {QStringLiteral("第二行"), {8429, 8444, 8473}},
                {QStringLiteral("第三行"), {8451, 8453, 8410}}};
    default:
        return {};
    }
}

QList<RuneRowDefinition> statShardRows()
{
    return {{QStringLiteral("进攻"), {5005, 5008, 5007}},
            {QStringLiteral("灵活"), {5008, 5002, 5003, 5010, 5012}},
            {QStringLiteral("防御"), {5001, 5013, 5011}}};
}

QString localizedRuneName(const int id)
{
    static const QHash<int, QString> names = {
        {8005, QStringLiteral("强攻")}, {8008, QStringLiteral("致命节奏")},
        {8021, QStringLiteral("迅捷步法")}, {8010, QStringLiteral("征服者")},
        {9101, QStringLiteral("吸收生命")}, {9111, QStringLiteral("凯旋")},
        {8009, QStringLiteral("气定神闲")}, {9104, QStringLiteral("传说：欢欣")},
        {9105, QStringLiteral("传说：急速")}, {9103, QStringLiteral("传说：血统")},
        {8014, QStringLiteral("致命一击")}, {8017, QStringLiteral("砍倒")},
        {8299, QStringLiteral("坚毅不倒")},
        {8112, QStringLiteral("电刑")}, {8124, QStringLiteral("掠食者")},
        {8128, QStringLiteral("黑暗收割")}, {9923, QStringLiteral("丛刃")},
        {8126, QStringLiteral("恶意中伤")}, {8139, QStringLiteral("血之滋味")},
        {8143, QStringLiteral("突然冲击")}, {8136, QStringLiteral("僵尸守卫")},
        {8137, QStringLiteral("深挖视野")}, {8138, QStringLiteral("眼球收集器")},
        {8105, QStringLiteral("无情猎手")}, {8106, QStringLiteral("终极猎手")},
        {8134, QStringLiteral("寻宝猎人")}, {8135, QStringLiteral("宝藏猎人")},
        {8214, QStringLiteral("召唤：艾黎")}, {8229, QStringLiteral("奥术彗星")},
        {8230, QStringLiteral("相位猛冲")}, {8224, QStringLiteral("奥术专注")},
        {8226, QStringLiteral("法力流系带")}, {8275, QStringLiteral("迅捷")},
        {8210, QStringLiteral("超然")}, {8234, QStringLiteral("灵光披风")},
        {8233, QStringLiteral("绝对专注")}, {8237, QStringLiteral("灼热")},
        {8236, QStringLiteral("风暴聚集")}, {8232, QStringLiteral("水上行走")},
        {8351, QStringLiteral("冰川增幅")}, {8360, QStringLiteral("启封的秘籍")},
        {8369, QStringLiteral("先攻")}, {8304, QStringLiteral("神奇之鞋")},
        {8306, QStringLiteral("海克斯科技闪现")}, {8313, QStringLiteral("三重 tonic")},
        {8345, QStringLiteral("饼干配送")}, {8347, QStringLiteral("星界洞悉")},
        {8316, QStringLiteral("全能通才")}, {8321, QStringLiteral("现金返还")},
        {8339, QStringLiteral("未来市场")}, {8352, QStringLiteral("时间扭曲补药")},
        {8437, QStringLiteral("不灭之握")}, {8439, QStringLiteral("余震")},
        {8465, QStringLiteral("守护者")}, {8446, QStringLiteral("爆破")},
        {8463, QStringLiteral("生命源泉")}, {8401, QStringLiteral("护盾猛击")},
        {8429, QStringLiteral("调节")}, {8444, QStringLiteral("复苏之风")},
        {8473, QStringLiteral("骸骨镀层")}, {8451, QStringLiteral("过度生长")},
        {8453, QStringLiteral("复苏")}, {8410, QStringLiteral("行近速率")},
        {5001, QStringLiteral("成长生命值")}, {5002, QStringLiteral("护甲")},
        {5003, QStringLiteral("魔法抗性")}, {5005, QStringLiteral("攻击速度")},
        {5007, QStringLiteral("技能急速")}, {5008, QStringLiteral("自适应之力")},
        {5010, QStringLiteral("移动速度")}, {5011, QStringLiteral("生命值")},
        {5012, QStringLiteral("成长抗性")}, {5013, QStringLiteral("韧性与减速抵抗")}
    };
    return names.value(id);
}

QString opggModePath(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    if (normalized == "ARAM") return QStringLiteral("aram");
    if (normalized == "ARENA") return QStringLiteral("arena");
    if (normalized == "URF") return QStringLiteral("urf");
    if (normalized == "ONEFORALL") return QStringLiteral("one-for-all");
    if (normalized == "ULTBOOK") return QStringLiteral("ultimate-spellbook");
    return {};
}

QString opggUrl(const QString &mode, const QString &lane, const QString &slug)
{
    if (slug.trimmed().isEmpty()) return {};
    const QString modePath = opggModePath(mode);
    if (!modePath.isEmpty()) return QStringLiteral("https://op.gg/zh-cn/lol/modes/%1/%2/build?region=kr&tier=emerald_plus")
        .arg(modePath, slug);
    QString routeLane = lane.trimmed().toLower();
    if (routeLane == QStringLiteral("bot")) routeLane = QStringLiteral("adc");
    const QString position = routeLane.isEmpty() ? QString{} : "/" + routeLane;
    const QString normalizedMode = mode.trimmed().toUpper();
    const QString type = normalizedMode == QStringLiteral("FLEXRANKED")
        ? QStringLiteral("flex")
        : normalizedMode == QStringLiteral("NORMAL") ? QStringLiteral("classic") : QStringLiteral("ranked");
    return QStringLiteral("https://op.gg/zh-cn/lol/champions/%1/build%2?position=%3&type=%4&region=kr&tier=emerald_plus")
        .arg(slug, position, routeLane.isEmpty() ? QStringLiteral("all") : routeLane, type);
}

QString modeLabel(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    if (normalized == QStringLiteral("SOLORANKED")) return QStringLiteral("排位 · 单双排");
    if (normalized == QStringLiteral("FLEXRANKED")) return QStringLiteral("排位 · 灵活组排");
    if (normalized == QStringLiteral("NORMAL")) return QStringLiteral("经典匹配");
    if (normalized == QStringLiteral("ARAM")) return QStringLiteral("大乱斗 · ARAM");
    if (normalized == QStringLiteral("ARENA")) return QStringLiteral("斗魂竞技场");
    if (normalized == QStringLiteral("URF")) return QStringLiteral("无限火力 · URF");
    if (normalized == QStringLiteral("ONEFORALL")) return QStringLiteral("终极魔典 · 一人之下");
    if (normalized == QStringLiteral("ULTBOOK")) return QStringLiteral("终极魔典");
    if (normalized == QStringLiteral("UNSUPPORTED")) return QStringLiteral("当前模式暂无 OP.GG 数据");
    return normalized.isEmpty() ? QStringLiteral("经典匹配") : normalized;
}

bool opggModeSupported(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    return normalized == QStringLiteral("SOLORANKED")
        || normalized == QStringLiteral("FLEXRANKED")
        || normalized == QStringLiteral("NORMAL")
        || normalized == QStringLiteral("ARENA")
        || normalized == QStringLiteral("ONEFORALL")
        || normalized == QStringLiteral("ULTBOOK");
}

} // namespace

OpggWindow::OpggWindow(ChampionRepository &champions, AssistController &controller,
                       GameFlowRepository &gameFlow, OpggCrawler &opgg, QWidget *owner)
    : QWidget(owner, Qt::Window),
      champions_(champions), controller_(controller), gameFlow_(gameFlow), opgg_(opgg), owner_(owner)
{
    setObjectName("opggWindow");
    setWindowTitle(QStringLiteral("OP.GG"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumSize(680, 620);
    resize(820, 900);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new QFrame(this);
    header->setObjectName("opggHeader");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 12, 10, 12);
    headerLayout->setSpacing(8);
    backButton_ = new QToolButton(header);
    backButton_->setObjectName("opggBack");
    backButton_->setToolTip(QStringLiteral("返回英雄排行"));
    backButton_->setAccessibleName(QStringLiteral("返回英雄排行"));
    backButton_->setIcon(backIcon());
    backButton_->setIconSize(QSize(22, 22));
    backButton_->setFixedSize(32, 32);
    backButton_->setAutoRaise(true);
    backButton_->setText(QString());
    backButton_->setVisible(false);
    headerLayout->addWidget(backButton_);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("OP.GG"), header);
    title->setObjectName("opggTitle");
    context_ = new QLabel(QStringLiteral("当前模式 · 默认分路"), header);
    context_->setObjectName("opggContext");
    titleColumn->addWidget(title);
    titleColumn->addWidget(context_);
    headerLayout->addLayout(titleColumn, 1);
    refreshButton_ = new QPushButton(QStringLiteral("刷新"), header);
    refreshButton_->setObjectName("secondary");
    refreshButton_->setToolTip(QStringLiteral("刷新当前模式和分路的英雄排行"));
    headerLayout->addWidget(refreshButton_);
    followButton_ = new QPushButton(QStringLiteral("跟随主窗口"), header);
    followButton_->setObjectName("secondary");
    followButton_->setCheckable(true);
    followButton_->setChecked(true);
    followButton_->setToolTip(QStringLiteral("关闭后可独立移动 OP.GG 窗口"));
    headerLayout->addWidget(followButton_);
    auto *close = new LineIconButton(LineIconButton::Icon::Close, QStringLiteral("关闭 OPGG 窗口"), header);
    close->setObjectName("opggClose");
    headerLayout->addWidget(close);
    root->addWidget(header);

    auto *filters = new QWidget(this);
    filters->setObjectName("opggFilters");
    auto *filterLayout = new QGridLayout(filters);
    filterLayout->setContentsMargins(14, 10, 14, 2);
    filterLayout->setHorizontalSpacing(7);
    filterLayout->setVerticalSpacing(6);
    auto *modeLabelWidget = new QLabel(QStringLiteral("模式"), filters);
    modeLabelWidget->setObjectName("opggFilterLabel");
    filterLayout->addWidget(modeLabelWidget, 0, 0);
    modeCombo_ = new QComboBox(filters);
    modeCombo_->setObjectName("opggModeCombo");
    modeCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filterLayout->addWidget(modeCombo_, 0, 1, 1, 3);
    auto *laneLabelWidget = new QLabel(QStringLiteral("分路"), filters);
    laneLabelWidget->setObjectName("opggFilterLabel");
    filterLayout->addWidget(laneLabelWidget, 0, 4);
    laneCombo_ = new QComboBox(filters);
    laneCombo_->setObjectName("opggLaneCombo");
    filterLayout->addWidget(laneCombo_, 0, 5);
    auto *rankLabelWidget = new QLabel(QStringLiteral("段位"), filters);
    rankLabelWidget->setObjectName("opggFilterLabel");
    filterLayout->addWidget(rankLabelWidget, 1, 0);
    rankCombo_ = new QComboBox(filters);
    rankCombo_->setObjectName("opggRankCombo");
    rankCombo_->setToolTip(QStringLiteral("选择韩服统计段位范围"));
    filterLayout->addWidget(rankCombo_, 1, 1);
    auto *versionLabelWidget = new QLabel(QStringLiteral("版本"), filters);
    versionLabelWidget->setObjectName("opggFilterLabel");
    filterLayout->addWidget(versionLabelWidget, 1, 2);
    versionCombo_ = new QComboBox(filters);
    versionCombo_->setObjectName("opggVersionCombo");
    versionCombo_->setToolTip(QStringLiteral("选择 OP.GG 数据版本"));
    filterLayout->addWidget(versionCombo_, 1, 3);
    auto *championLabelWidget = new QLabel(QStringLiteral("英雄"), filters);
    championLabelWidget->setObjectName("opggFilterLabel");
    filterLayout->addWidget(championLabelWidget, 2, 0);
    selectedChampionLabel_ = new QLabel(QStringLiteral("未选择英雄"), filters);
    selectedChampionLabel_->setObjectName("opggSelectedChampion");
    selectedChampionLabel_->setWordWrap(true);
    filterLayout->addWidget(selectedChampionLabel_, 2, 1, 1, 3);
    chooseChampionButton_ = new QPushButton(QStringLiteral("选择英雄"), filters);
    chooseChampionButton_->setObjectName("secondary");
    chooseChampionButton_->setToolTip(QStringLiteral("使用英雄选择器选择当前模式可用英雄"));
    filterLayout->addWidget(chooseChampionButton_, 2, 4, 1, 2);
    filterLayout->setColumnStretch(3, 1);
    root->addWidget(filters);

    contentStack_ = new QStackedWidget(this);
    contentStack_->setObjectName("opggContentStack");
    rankingPage_ = new QWidget(contentStack_);
    auto *rankingPageLayout = new QVBoxLayout(rankingPage_);
    rankingPageLayout->setContentsMargins(14, 14, 14, 14);
    rankingPageLayout->setSpacing(12);

    auto *rankingPanel = new QFrame(rankingPage_);
    rankingPanel->setObjectName("opggPanel");
    auto *rankingLayout = new QVBoxLayout(rankingPanel);
    rankingLayout->setContentsMargins(12, 10, 12, 12);
    rankingLayout->setSpacing(8);
    auto *rankingTitle = new QLabel(QStringLiteral("英雄强度排行"), rankingPanel);
    rankingTitle->setObjectName("opggSectionTitle");
    rankingLayout->addWidget(rankingTitle);
    selectedRankingLabel_ = new QLabel(QStringLiteral("尚未选择英雄；点击“选择英雄”查看该英雄在当前筛选条件下的排行。"), rankingPanel);
    selectedRankingLabel_->setObjectName("opggSelectedRanking");
    selectedRankingLabel_->setWordWrap(true);
    rankingLayout->addWidget(selectedRankingLabel_);
    rankingTable_ = new QTableWidget(rankingPanel);
    rankingTable_->setObjectName("opggRankingTable");
    rankingTable_->setColumnCount(8);
    rankingTable_->setHorizontalHeaderLabels({QStringLiteral("排名"), QStringLiteral("英雄头像"), QStringLiteral("强度"),
                                              QStringLiteral("位置"), QStringLiteral("胜率"), QStringLiteral("选取率"),
                                              QStringLiteral("禁用率"), QStringLiteral("劣势对抗")});
    rankingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rankingTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rankingTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    rankingTable_->setAlternatingRowColors(true);
    rankingTable_->verticalHeader()->setVisible(false);
    rankingTable_->horizontalHeader()->setStretchLastSection(true);
    rankingTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    rankingTable_->setIconSize(QSize(30, 30));
    rankingTable_->verticalHeader()->setDefaultSectionSize(38);
    rankingTable_->setMinimumHeight(340);
    rankingTable_->setShowGrid(false);
    rankingLayout->addWidget(rankingTable_);
    rankingPageLayout->addWidget(rankingPanel, 1);
    contentStack_->addWidget(rankingPage_);

    detailPage_ = new QWidget;
    detailPage_->setObjectName("opggDetailPage");
    detailPage_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *detailLayout = new QVBoxLayout(detailPage_);
    detailLayout->setContentsMargins(14, 14, 14, 14);
    detailLayout->setSpacing(10);
    auto *detailHeader = new QFrame(detailPage_);
    detailHeader->setObjectName("opggPanel");
    auto *detailHeaderLayout = new QGridLayout(detailHeader);
    detailHeaderLayout->setContentsMargins(12, 10, 12, 10);
    detailTitle_ = new QLabel(QStringLiteral("英雄推荐配置"), detailHeader);
    detailTitle_->setObjectName("opggSectionTitle");
    detailHeaderLayout->addWidget(detailTitle_, 0, 0, 1, 3);
    detailMeta_ = new QLabel(QStringLiteral("选择英雄后读取韩服数据"), detailHeader);
    detailMeta_->setObjectName("opggDetails");
    detailMeta_->setWordWrap(true);
    detailHeaderLayout->addWidget(detailMeta_, 1, 0, 1, 3);
    auto *detailSummary = new QWidget(detailHeader);
    detailSummary->setObjectName("opggDetailSummary");
    auto *summaryLayout = new QHBoxLayout(detailSummary);
    summaryLayout->setContentsMargins(0, 4, 0, 4);
    summaryLayout->setSpacing(10);
    detailLaneStats_ = new QLabel(QStringLiteral("分路：--"), detailSummary);
    detailLaneStats_->setObjectName("opggDetailMetric");
    detailLaneStats_->setWordWrap(true);
    summaryLayout->addWidget(detailLaneStats_, 1);
    detailRankIcon_ = new QLabel(detailSummary);
    detailRankIcon_->setObjectName("opggRankIcon");
    detailRankIcon_->setFixedSize(30, 30);
    detailRankIcon_->setAlignment(Qt::AlignCenter);
    summaryLayout->addWidget(detailRankIcon_);
    detailRankText_ = new QLabel(QStringLiteral("段位：--"), detailSummary);
    detailRankText_->setObjectName("opggDetailMetric");
    detailRankText_->setWordWrap(true);
    detailRankText_->setMinimumWidth(72);
    summaryLayout->addWidget(detailRankText_);
    detailRole_ = new QLabel(QStringLiteral("定位：--"), detailSummary);
    detailRole_->setObjectName("opggDetailMetric");
    detailRole_->setWordWrap(true);
    summaryLayout->addWidget(detailRole_, 1);
    detailVersion_ = new QLabel(QStringLiteral("版本：--"), detailSummary);
    detailVersion_->setObjectName("opggDetailMetric");
    detailVersion_->setWordWrap(true);
    summaryLayout->addWidget(detailVersion_, 1);
    detailHeaderLayout->addWidget(detailSummary, 2, 0, 1, 3);
    auto *detailLaneLabel = new QLabel(QStringLiteral("可切换分路"), detailHeader);
    detailLaneLabel->setObjectName("opggFilterLabel");
    detailHeaderLayout->addWidget(detailLaneLabel, 3, 0);
    detailLaneCombo_ = new QComboBox(detailHeader);
    detailLaneCombo_->setObjectName("opggDetailLaneCombo");
    detailLaneCombo_->setMinimumWidth(112);
    detailLaneCombo_->setToolTip(QStringLiteral("切换该英雄的 OP.GG 推荐配置分路"));
    detailHeaderLayout->addWidget(detailLaneCombo_, 3, 1, 1, 2);
    detailHeaderLayout->setColumnStretch(1, 1);
    detailLayout->addWidget(detailHeader);

    auto *runePanel = new QFrame(detailPage_);
    runePanel->setObjectName("opggPanel");
    auto *runeLayout = new QVBoxLayout(runePanel);
    runeLayout->setContentsMargins(12, 10, 12, 10);
    auto *runeHeading = new QHBoxLayout;
    auto *runeHeadingLabel = new QLabel(QStringLiteral("推荐符文与召唤师技能"), runePanel);
    runeHeadingLabel->setObjectName("opggSectionTitle");
    runeHeading->addWidget(runeHeadingLabel);
    runeHeading->addStretch();
    runeVariantCombo_ = new QComboBox(runePanel);
    runeVariantCombo_->setObjectName("opggVariantCombo");
    runeVariantCombo_->setMinimumWidth(150);
    runeVariantCombo_->setToolTip(QStringLiteral("切换 OP.GG 推荐符文方案"));
    runeHeading->addWidget(runeVariantCombo_);
    runeLayout->addLayout(runeHeading);
    detailRunes_ = new QWidget(runePanel);
    detailRunes_->setObjectName("opggRuneContent");
    detailRunes_->setLayout(new QVBoxLayout);
    runeLayout->addWidget(detailRunes_);
    setContentPlaceholder(detailRunes_, QStringLiteral("等待读取…"));
    detailLayout->addWidget(runePanel);

    auto *skillPanel = new QFrame(detailPage_);
    skillPanel->setObjectName("opggPanel");
    auto *skillLayout = new QVBoxLayout(skillPanel);
    skillLayout->setContentsMargins(12, 10, 12, 10);
    auto *skillHeading = new QLabel(QStringLiteral("推荐加点顺序"), skillPanel);
    skillHeading->setObjectName("opggSectionTitle");
    skillLayout->addWidget(skillHeading);
    detailSkills_ = new QWidget(skillPanel);
    detailSkills_->setObjectName("opggSkillContent");
    detailSkills_->setLayout(new QVBoxLayout);
    skillLayout->addWidget(detailSkills_);
    setContentPlaceholder(detailSkills_, QStringLiteral("等待读取…"));
    detailLayout->addWidget(skillPanel);

    auto *itemPanel = new QFrame(detailPage_);
    itemPanel->setObjectName("opggPanel");
    auto *itemLayout = new QVBoxLayout(itemPanel);
    itemLayout->setContentsMargins(12, 10, 12, 10);
    auto *itemHeading = new QHBoxLayout;
    auto *itemHeadingLabel = new QLabel(QStringLiteral("推荐出装（按顺序）"), itemPanel);
    itemHeadingLabel->setObjectName("opggSectionTitle");
    itemHeading->addWidget(itemHeadingLabel);
    itemHeading->addStretch();
    itemVariantCombo_ = new QComboBox(itemPanel);
    itemVariantCombo_->setObjectName("opggVariantCombo");
    itemVariantCombo_->setMinimumWidth(150);
    itemVariantCombo_->setToolTip(QStringLiteral("切换 OP.GG 推荐出装方案"));
    itemHeading->addWidget(itemVariantCombo_);
    itemLayout->addLayout(itemHeading);
    detailItems_ = new QWidget(itemPanel);
    detailItems_->setObjectName("opggItemContent");
    detailItems_->setLayout(new QVBoxLayout);
    itemLayout->addWidget(detailItems_);
    setContentPlaceholder(detailItems_, QStringLiteral("等待读取…"));
    detailLayout->addWidget(itemPanel);

    auto *counterPanel = new QFrame(detailPage_);
    counterPanel->setObjectName("opggPanel");
    auto *counterLayout = new QVBoxLayout(counterPanel);
    counterLayout->setContentsMargins(12, 10, 12, 10);
    auto *counterHeading = new QLabel(QStringLiteral("Counter / 克制关系"), counterPanel);
    counterHeading->setObjectName("opggSectionTitle");
    counterLayout->addWidget(counterHeading);
    detailCounters_ = new QWidget(counterPanel);
    detailCounters_->setObjectName("opggCounterContent");
    detailCounters_->setLayout(new QVBoxLayout);
    counterLayout->addWidget(detailCounters_);
    setContentPlaceholder(detailCounters_, QStringLiteral("等待读取…"));
    detailLayout->addWidget(counterPanel);

    auto *buildActions = new QHBoxLayout;
    buildActions->setContentsMargins(0, 0, 0, 0);
    applyButton_ = new QPushButton(QStringLiteral("一键配置符文预设"), detailPage_);
    applyButton_->setObjectName("primary");
    applyButton_->setEnabled(false);
    buildActions->addWidget(applyButton_);
    openLinkButton_ = new QPushButton(QStringLiteral("打开 OP.GG"), detailPage_);
    openLinkButton_->setObjectName("secondary");
    openLinkButton_->setEnabled(false);
    buildActions->addWidget(openLinkButton_);
    buildActions->addStretch();
    detailLayout->addLayout(buildActions);
    // These aliases keep the existing build-ready/error flow shared with the
    // Assist page while the detail view owns the visible text.
    buildTitle_ = detailTitle_;
    buildDetails_ = detailMeta_;
    detailScroll_ = new QScrollArea(contentStack_);
    detailScroll_->setObjectName("opggDetailScroll");
    detailScroll_->setWidgetResizable(true);
    detailScroll_->setFrameShape(QFrame::NoFrame);
    detailScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    detailScroll_->setWidget(detailPage_);
    contentStack_->addWidget(detailScroll_);

    status_ = new QLabel(QStringLiteral("点击刷新读取韩服排行"), this);
    status_->setObjectName("opggStatus");
    status_->setWordWrap(true);
    status_->setMinimumHeight(22);
    status_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    status_->setContentsMargins(14, 0, 14, 0);
    root->addWidget(contentStack_, 1);
    root->addWidget(status_);

    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    connect(backButton_, &QToolButton::clicked, this, &OpggWindow::showRankingPage);
    connect(refreshButton_, &QPushButton::clicked, this, &OpggWindow::refreshRanking);
    connect(followButton_, &QPushButton::toggled, this, [this](const bool follow) {
        setFollowOwner(follow);
    });
    connect(chooseChampionButton_, &QPushButton::clicked, this, &OpggWindow::openChampionPicker);
    connect(applyButton_, &QPushButton::clicked, this, [this] {
        if (!build_.isValid() || buildRequestInFlight_) return;
        applyButton_->setEnabled(false);
        status_->setText(QStringLiteral("正在写入当前符文预设和召唤师技能…"));
        const QPointer<OpggWindow> window(this);
        controller_.applyBuild(build_, [window](bool success, const QString &message) {
            if (!window) return;
            window->applyButton_->setEnabled(window->build_.isValid());
            window->status_->setText(success
                ? QStringLiteral("推荐配置已应用")
                : QStringLiteral("推荐配置应用失败：") + message);
            if (!message.trimmed().isEmpty()) window->buildDetails_->setToolTip(message);
        });
    });
    connect(openLinkButton_, &QPushButton::clicked, this, [this] {
        const QString slug = championSlug(champions_, build_.championId);
        const QString url = opggUrl(lockedMode_, lockedLane_, slug);
        if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
    });
    connect(runeVariantCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index) { if (!syncingFilters_) applyRuneVariant(index); });
    connect(itemVariantCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index) { if (!syncingFilters_) applyItemVariant(index); });
    connect(&controller_, &AssistController::championLocked, this,
            [this](int championId, const QString &lane, const QString &mode) {
                const bool manualFilter = !automaticMode_.isEmpty()
                    && (selectedMode().compare(automaticMode_, Qt::CaseInsensitive) != 0
                        || selectedLane().compare(automaticLane_, Qt::CaseInsensitive) != 0);
                handleChampionLocked(championId, manualFilter ? selectedLane() : lane,
                                     manualFilter ? selectedMode() : mode, true);
            });
    connect(&champions_, &ChampionRepository::championsChanged, this, [this] {
        updateChampionPickerState();
        renderRanking(ranking_);
    });
    connect(&champions_, &ChampionRepository::championPortraitsChanged, this, [this] {
        updateChampionPickerState();
        renderRanking(ranking_);
    });
    connect(&champions_, &ChampionRepository::staticIconsChanged, this, [this] {
        if (build_.isValid()) renderBuild(build_);
        renderRanking(ranking_);
    });

    populateModeOptions();
    populateRankOptions();
    populateVersionOptions();
    updateChampionPickerState();
    populateLaneOptions(QStringLiteral("NORMAL"), QStringLiteral("default"));
    connect(modeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (syncingFilters_) return;
        const QString mode = selectedMode();
        const QString lane = selectedLane();
        syncingFilters_ = true;
        populateLaneOptions(mode, lane);
        resetModeChampionPool(mode);
        updateChampionPickerState();
        syncingFilters_ = false;
        updateContextLabels();
        if (lockedChampionId_ > 0) {
            applySelectedChampion(lockedChampionId_);
        }
        refreshRanking();
    });
    connect(laneCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (syncingFilters_) return;
        updateContextLabels();
        if (lockedChampionId_ > 0) applySelectedChampion(lockedChampionId_);
        refreshRanking();
    });
    connect(rankCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (syncingFilters_) return;
        refreshRanking();
        if (lockedChampionId_ > 0 && contentStack_->currentWidget() != rankingPage_) {
            build_ = {};
            buildAttempted_ = false;
            fetchBuild(lockedChampionId_, lockedLane_, lockedMode_.isEmpty() ? selectedMode() : lockedMode_);
        }
    });
    connect(versionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (syncingFilters_) return;
        refreshRanking();
        if (lockedChampionId_ > 0 && contentStack_->currentWidget() != rankingPage_) {
            build_ = {};
            buildAttempted_ = false;
            fetchBuild(lockedChampionId_, lockedLane_, lockedMode_.isEmpty() ? selectedMode() : lockedMode_);
        }
    });
    connect(rankingTable_, &QTableWidget::cellClicked, this, [this](const int row, const int column) {
        Q_UNUSED(column);
        if (!rankingTable_ || row < 0 || row >= rankingTable_->rowCount()) return;
        const QTableWidgetItem *item = rankingTable_->item(row, 1);
        if (!item) return;
        const int championId = item->data(Qt::UserRole).toInt();
        if (championId > 0) openChampionDetail(championId);
    });

    connect(detailLaneCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (syncingFilters_ || index < 0 || lockedChampionId_ <= 0) return;
        const QString lane = detailLaneCombo_->itemData(index).toString();
        lockedLane_ = lane.isEmpty() ? QStringLiteral("default") : lane;
        // Keep the global filter in sync so periodic game-flow snapshots do
        // not snap the detail view back to the automatically detected lane.
        const int filterLaneIndex = laneCombo_->findData(lockedLane_);
        if (filterLaneIndex >= 0 && laneCombo_->currentIndex() != filterLaneIndex) {
            QSignalBlocker filterBlocker(laneCombo_);
            laneCombo_->setCurrentIndex(filterLaneIndex);
        }
        updateContextLabels();
        build_ = {};
        buildAttempted_ = false;
        buildTitle_->setText(QStringLiteral("%1 · 正在读取 OP.GG 推荐配置")
            .arg(championName(champions_, lockedChampionId_)));
        buildDetails_->setText(QStringLiteral("模式：%1 · 分路：%2")
            .arg(modeLabel(lockedMode_.isEmpty() ? selectedMode() : lockedMode_), AssistController::laneLabel(lockedLane_)));
        setContentPlaceholder(detailRunes_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailSkills_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailItems_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailCounters_, QStringLiteral("等待读取…"));
        applyButton_->setEnabled(false);
        openLinkButton_->setEnabled(false);
        fetchBuild(lockedChampionId_, lockedLane_, lockedMode_.isEmpty() ? selectedMode() : lockedMode_);
        refreshRanking();
    });

    setSnapshot(gameFlow_.snapshot());
}

void OpggWindow::populateModeOptions()
{
    const QString current = selectedMode();
    syncingFilters_ = true;
    modeCombo_->clear();
    const QList<QPair<QString, QString>> modes = {
        {QStringLiteral("排位 · 单双排"), QStringLiteral("SOLORANKED")},
        {QStringLiteral("排位 · 灵活组排"), QStringLiteral("FLEXRANKED")},
        {QStringLiteral("经典匹配"), QStringLiteral("NORMAL")},
        {QStringLiteral("斗魂竞技场"), QStringLiteral("ARENA")},
        {QStringLiteral("无限乱斗"), QStringLiteral("ONEFORALL")},
        {QStringLiteral("终极魔典"), QStringLiteral("ULTBOOK")}
    };
    for (const auto &mode : modes) modeCombo_->addItem(mode.first, mode.second);
    if (!current.isEmpty() && opggModeSupported(current) && modeCombo_->findData(current) < 0) {
        modeCombo_->addItem(modeLabel(current), current);
    }
    const int index = opggModeSupported(current) ? modeCombo_->findData(current) : -1;
    modeCombo_->setCurrentIndex(index >= 0 ? index : modeCombo_->findData(QStringLiteral("NORMAL")));
    syncingFilters_ = false;
}

void OpggWindow::populateLaneOptions(const QString &mode, const QString &preferredLane)
{
    const QString normalizedMode = mode.trimmed().toUpper();
    const QString normalizedPreferred = AssistController::normalizeLane(preferredLane).isEmpty()
        ? QStringLiteral("default") : AssistController::normalizeLane(preferredLane);
    const bool available = modeSupportsLane(normalizedMode);
    syncingFilters_ = true;
    laneCombo_->clear();
    if (available) {
        laneCombo_->addItem(QStringLiteral("全部分路"), QStringLiteral("default"));
        laneCombo_->addItem(QStringLiteral("上路"), QStringLiteral("top"));
        laneCombo_->addItem(QStringLiteral("打野"), QStringLiteral("jungle"));
        laneCombo_->addItem(QStringLiteral("中路"), QStringLiteral("mid"));
        laneCombo_->addItem(QStringLiteral("下路"), QStringLiteral("bot"));
        laneCombo_->addItem(QStringLiteral("辅助"), QStringLiteral("support"));
    } else {
        laneCombo_->addItem(QStringLiteral("模式默认"), QStringLiteral("default"));
    }
    const int preferredIndex = laneCombo_->findData(normalizedPreferred);
    laneCombo_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
    laneCombo_->setEnabled(available);
    laneCombo_->setToolTip(available
        ? QStringLiteral("按 OPGG 分路筛选英雄排行")
        : QStringLiteral("该游戏模式没有分路排行，使用模式默认设置"));
    syncingFilters_ = false;
}

void OpggWindow::populateDetailLaneOptions(const int championId, const QString &mode,
                                           const QString &preferredLane)
{
    Q_UNUSED(championId);
    QSignalBlocker blocker(detailLaneCombo_);
    detailLaneCombo_->clear();
    if (!modeSupportsLane(mode)) {
        detailLaneCombo_->addItem(QStringLiteral("模式默认"), QStringLiteral("default"));
        detailLaneCombo_->setEnabled(false);
        return;
    }

    // Keep every lane that OP.GG can query visible. Local role metadata is
    // used only as a hint; it is often a class tag (for example MAGE) rather
    // than a lane, and must not hide an off-meta build from the user.
    const QList<QPair<QString, QString>> allLanes = {
        {QStringLiteral("全部分路"), QStringLiteral("default")},
        {QStringLiteral("上路"), QStringLiteral("top")},
        {QStringLiteral("打野"), QStringLiteral("jungle")},
        {QStringLiteral("中路"), QStringLiteral("mid")},
        {QStringLiteral("下路"), QStringLiteral("bot")},
        {QStringLiteral("辅助"), QStringLiteral("support")}
    };
    QString normalizedPreferred = AssistController::normalizeLane(preferredLane);
    if (normalizedPreferred.isEmpty()) normalizedPreferred = QStringLiteral("default");

    // `Champion::roles` contains OP.GG class/type labels (AP输出、辅助、AD
    // 战士...), not lanes.  Always expose the real OP.GG lane query options;
    // the build response supplies the usage percentages independently.
    for (const auto &lane : allLanes) detailLaneCombo_->addItem(lane.first, lane.second);
    const int preferredIndex = detailLaneCombo_->findData(normalizedPreferred);
    detailLaneCombo_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
    detailLaneCombo_->setEnabled(true);
}

void OpggWindow::updateChampionPickerState()
{
    const bool restricted = modeUsesRestrictedChampionPool(selectedMode());
    if (restricted != modeChampionPoolRestricted_) {
        modeChampionPoolRestricted_ = restricted;
        modeChampionPoolReady_ = !restricted;
        if (!restricted) modeChampionIds_.clear();
    }
    if (selectedChampionLabel_) {
        QStringList selectedNames;
        for (const int id : selectedChampionIds_) {
            if (id <= 0) continue;
            selectedNames.append(QStringLiteral("%1（%2）")
                .arg(championName(champions_, id), championTypeText(champions_, id).mid(3)));
        }
        if (selectedNames.isEmpty() && lockedChampionId_ > 0) {
            selectedNames.append(QStringLiteral("%1（%2）")
                .arg(championName(champions_, lockedChampionId_), championTypeText(champions_, lockedChampionId_).mid(3)));
        }
        selectedChampionLabel_->setText(selectedNames.isEmpty()
            ? QStringLiteral("未选择英雄")
            : QStringLiteral("已选择 %1 名：%2 · 点击右侧按钮切换")
                .arg(selectedNames.size()).arg(selectedNames.join(QStringLiteral("、"))));
    }
    if (chooseChampionButton_) {
        const bool ready = !restricted || modeChampionPoolReady_;
        chooseChampionButton_->setEnabled(ready);
        chooseChampionButton_->setToolTip(ready
            ? QStringLiteral("使用英雄选择器选择当前模式可用英雄")
            : QStringLiteral("正在读取当前模式英雄池，请稍候"));
    }
    renderSelectedRanking();
}

void OpggWindow::populateRankOptions()
{
    if (!rankCombo_) return;
    const QString current = selectedRankTier();
    QSignalBlocker blocker(rankCombo_);
    rankCombo_->clear();
    const QList<QPair<QString, QString>> tiers = {
        {QStringLiteral("全部段位"), QStringLiteral("all")},
        {QStringLiteral("白银以上"), QStringLiteral("silver_plus")},
        {QStringLiteral("黄金以上"), QStringLiteral("gold_plus")},
        {QStringLiteral("铂金以上"), QStringLiteral("platinum_plus")},
        {QStringLiteral("翡翠以上"), QStringLiteral("emerald_plus")},
        {QStringLiteral("钻石以上"), QStringLiteral("diamond_plus")},
        {QStringLiteral("大师以上"), QStringLiteral("master_plus")},
        {QStringLiteral("王者"), QStringLiteral("challenger")}
    };
    for (const auto &tier : tiers) rankCombo_->addItem(tier.first, tier.second);
    const int index = rankCombo_->findData(current);
    rankCombo_->setCurrentIndex(index >= 0 ? index : rankCombo_->findData(QStringLiteral("emerald_plus")));
}

void OpggWindow::populateVersionOptions()
{
    if (!versionCombo_) return;
    const QString current = selectedVersion();
    QSignalBlocker blocker(versionCombo_);
    versionCombo_->clear();
    versionCombo_->addItem(QStringLiteral("当前版本"), QString{});
    // Keep a short, explicit history so requests remain reproducible even if
    // the remote version list is unavailable.
    for (const QString &version : {QStringLiteral("16.18"), QStringLiteral("16.17"), QStringLiteral("16.16"), QStringLiteral("16.15"), QStringLiteral("16.14")}) {
        versionCombo_->addItem(version, version);
    }
    const int index = versionCombo_->findData(current);
    versionCombo_->setCurrentIndex(index >= 0 ? index : 0);
}

void OpggWindow::updateDetectedVersion(const QString &version)
{
    const QString normalized = version.trimmed();
    if (normalized.isEmpty() || !versionCombo_) return;
    detectedVersion_ = normalized;
    QSignalBlocker blocker(versionCombo_);
    const int existing = versionCombo_->findData(normalized);
    if (existing < 0) versionCombo_->insertItem(1, normalized, normalized);
    versionCombo_->setItemText(0, QStringLiteral("当前版本（%1）").arg(normalized));
}

void OpggWindow::updateModeChampionPool()
{
    const QString mode = selectedMode();
    modeChampionPoolRestricted_ = modeUsesRestrictedChampionPool(mode);
    modeChampionPoolReady_ = !modeChampionPoolRestricted_;
    modeChampionIds_.clear();
    // Classic queues share the complete base champion catalog. Their OPGG
    // response is lane-scoped and usually contains only the ranked subset,
    // which is not the same thing as the set of champions the client allows.
    if (mode == QStringLiteral("NORMAL") || mode == QStringLiteral("SOLORANKED")
        || mode == QStringLiteral("FLEXRANKED")) {
        updateChampionPickerState();
        return;
    }
    for (const OpggChampionRank &entry : ranking_) {
        const int id = championIdForRank(champions_, entry);
        if (id > 0) modeChampionIds_.insert(id);
    }
    modeChampionPoolReady_ = !modeChampionIds_.isEmpty();
    updateChampionPickerState();
}

void OpggWindow::resetModeChampionPool(const QString &mode)
{
    modeChampionPoolRestricted_ = modeUsesRestrictedChampionPool(mode);
    modeChampionIds_.clear();
    if (!modeChampionPoolRestricted_) {
        modeChampionPoolReady_ = true;
        return;
    }

    // A game-flow poll can rebuild the filter controls without changing the
    // selected OPGG query. Reuse a matching cached ranking instead of
    // briefly (or permanently) dropping the mode-specific pool.
    const bool cachedForQuery = rankingMode_.compare(mode, Qt::CaseInsensitive) == 0
        && rankingLane_.compare(selectedLane(), Qt::CaseInsensitive) == 0
        && rankingTier_.compare(selectedRankTier(), Qt::CaseInsensitive) == 0
        && rankingVersion_.compare(selectedVersion(), Qt::CaseInsensitive) == 0
        && !ranking_.isEmpty();
    if (!cachedForQuery) {
        modeChampionPoolReady_ = false;
        return;
    }
    for (const OpggChampionRank &entry : ranking_) {
        const int id = championIdForRank(champions_, entry);
        if (id > 0) modeChampionIds_.insert(id);
    }
    modeChampionPoolReady_ = !modeChampionIds_.isEmpty();
}

bool OpggWindow::modeUsesRestrictedChampionPool(const QString &mode) const
{
    const QString normalized = mode.trimmed().toUpper();
    return normalized != QStringLiteral("NORMAL")
        && normalized != QStringLiteral("SOLORANKED")
        && normalized != QStringLiteral("FLEXRANKED");
}

void OpggWindow::syncFiltersToSnapshot()
{
    const QString snapshotMode = AssistController::modeKey(snapshot_);
    // ARAM/URF are valid LCU queue labels but are not exposed by the OP.GG
    // champion endpoint used here.  Keep them out of the selectable list and
    // show an explicit no-data state instead of issuing a bogus request.
    const QString mode = opggModeSupported(snapshotMode) ? snapshotMode : QStringLiteral("UNSUPPORTED");
    const QString lane = mode == QStringLiteral("UNSUPPORTED")
        ? QStringLiteral("default") : AssistController::effectiveLane(snapshot_);
    const QString currentMode = selectedMode();
    const QString currentLane = selectedLane();
    const bool actualModeChanged = automaticMode_.isEmpty()
        || automaticMode_.compare(mode, Qt::CaseInsensitive) != 0;
    const bool userOverrodeFilters = !automaticMode_.isEmpty()
        && (currentMode.compare(automaticMode_, Qt::CaseInsensitive) != 0
            || currentLane.compare(automaticLane_, Qt::CaseInsensitive) != 0);
    const bool adoptSnapshot = actualModeChanged || !userOverrodeFilters;

    syncingFilters_ = true;
    if (adoptSnapshot) {
        int modeIndex = modeCombo_->findData(mode);
        if (modeIndex < 0) {
            modeCombo_->addItem(mode == QStringLiteral("UNSUPPORTED")
                                    ? QStringLiteral("当前模式暂无 OP.GG 数据") : modeLabel(mode), mode);
            modeIndex = modeCombo_->findData(mode);
        }
        modeCombo_->setCurrentIndex(modeIndex);
        populateLaneOptions(mode, lane);
        resetModeChampionPool(mode);
        updateChampionPickerState();
    } else {
        populateLaneOptions(currentMode, currentLane);
    }
    syncingFilters_ = false;
    automaticMode_ = mode;
    automaticLane_ = lane;
}

void OpggWindow::applySelectedChampion(const int championId, const bool revealWindow)
{
    if (championId <= 0) {
        if (lockedChampionId_ <= 0 && selectedChampionIds_.isEmpty()) return;
        lockedChampionId_ = 0;
        selectedChampionIds_.clear();
        lockedLane_.clear();
        lockedMode_.clear();
        build_ = {};
        buildAttempted_ = false;
        buildTitle_->setText(QStringLiteral("锁定英雄后显示推荐配置"));
        buildDetails_->setText(QStringLiteral("符文、属性碎片和召唤师技能会按当前模式与分路从 OP.GG 获取。"));
        setContentPlaceholder(detailRunes_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailSkills_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailItems_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailCounters_, QStringLiteral("等待读取…"));
        applyButton_->setEnabled(false);
        openLinkButton_->setEnabled(false);
        updateChampionPickerState();
        return;
    }
    if (!selectedChampionIds_.contains(championId)) {
        selectedChampionIds_.clear();
        selectedChampionIds_.append(championId);
    }
    handleChampionLocked(championId, selectedLane(), selectedMode(), revealWindow);
}

void OpggWindow::openChampionPicker()
{
    if (modeChampionPoolRestricted_ && !modeChampionPoolReady_) {
        status_->setText(QStringLiteral("正在读取当前模式英雄池，请稍候再选择英雄"));
        return;
    }
    ChampionPicker picker(champions_, this);
    picker.setAllowedChampionIds(modeChampionIds_);
    picker.setPoolRestricted(modeChampionPoolRestricted_);
    // The picker is intentionally role-based.  The OP.GG window's lane
    // selector controls the ranking/build query, while the picker keeps its
    // independent "英雄定位" filter.  Passing the lane filter here used to
    // hide that selector and made roles and lanes appear interchangeable.
    QList<int> currentSelection = selectedChampionIds_;
    if (currentSelection.isEmpty() && lockedChampionId_ > 0) currentSelection.append(lockedChampionId_);
    picker.setSelectedChampionIds(currentSelection);
    if (picker.exec() != QDialog::Accepted) return;
    const QList<int> selected = picker.selectedChampionIds();
    if (selected.isEmpty()) {
        applySelectedChampion(0);
        showRankingPage();
        renderRanking(ranking_);
        return;
    }
    selectedChampionIds_ = selected;
    if (selected.size() == 1) {
        openChampionDetail(selected.constFirst());
    } else {
        // Multi-select is a ranking comparison workflow. Keep the ranking
        // page visible instead of replacing it with only the last detail.
        if (lockedChampionId_ > 0) {
            lockedChampionId_ = 0;
            lockedLane_.clear();
            lockedMode_.clear();
            build_ = {};
            buildAttempted_ = false;
            applyButton_->setEnabled(false);
            openLinkButton_->setEnabled(false);
        }
        showRankingPage();
        updateChampionPickerState();
        renderRanking(ranking_);
    }
}

void OpggWindow::openChampionDetail(const int championId)
{
    if (championId <= 0) return;
    showDetailPage();
    applySelectedChampion(championId);
}

void OpggWindow::showRankingPage()
{
    if (contentStack_ && rankingPage_) contentStack_->setCurrentWidget(rankingPage_);
    if (backButton_) backButton_->setVisible(false);
}

void OpggWindow::showDetailPage()
{
    if (contentStack_ && detailScroll_) contentStack_->setCurrentWidget(detailScroll_);
    if (backButton_) backButton_->setVisible(true);
}

QString OpggWindow::selectedMode() const
{
    const QString value = modeCombo_ ? modeCombo_->currentData().toString().trimmed().toUpper() : QString{};
    return value.isEmpty() ? QStringLiteral("NORMAL") : value;
}

QString OpggWindow::selectedLane() const
{
    const QString value = laneCombo_ ? laneCombo_->currentData().toString().trimmed().toLower() : QString{};
    return value.isEmpty() ? QStringLiteral("default") : value;
}

QString OpggWindow::selectedRankTier() const
{
    return rankCombo_ ? rankCombo_->currentData().toString().trimmed().toLower() : QStringLiteral("emerald_plus");
}

QString OpggWindow::selectedVersion() const
{
    return versionCombo_ ? versionCombo_->currentData().toString().trimmed() : QString{};
}

bool OpggWindow::modeSupportsLane(const QString &mode) const
{
    const QString normalized = mode.trimmed().toUpper();
    return normalized == QStringLiteral("NORMAL")
        || normalized == QStringLiteral("SOLORANKED")
        || normalized == QStringLiteral("FLEXRANKED");
}

void OpggWindow::setSnapshot(const GameFlowSnapshot &snapshot)
{
    const QString oldMode = selectedMode();
    const QString oldLane = selectedLane();
    const QString oldPhase = snapshot_.phase;
    snapshot_ = snapshot;
    syncFiltersToSnapshot();
    updateContextLabels();

    const QString mode = selectedMode();
    const QString lane = selectedLane();
    const bool phaseChanged = oldPhase.compare(snapshot_.phase, Qt::CaseInsensitive) != 0;
    const bool enteringOrLeavingChampSelect = phaseChanged
        && (oldPhase.compare(QStringLiteral("ChampSelect"), Qt::CaseInsensitive) == 0
            || snapshot_.phase.compare(QStringLiteral("ChampSelect"), Qt::CaseInsensitive) == 0);
    if (enteringOrLeavingChampSelect) {
        lockedChampionId_ = 0;
        selectedChampionIds_.clear();
        lockedLane_.clear();
        lockedMode_.clear();
        build_ = {};
        buildAttempted_ = false;
        buildTitle_->setText(QStringLiteral("锁定英雄后显示推荐配置"));
        buildDetails_->setText(QStringLiteral("符文、属性碎片和召唤师技能会按当前模式与分路从 OP.GG 获取。"));
        setContentPlaceholder(detailRunes_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailSkills_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailItems_, QStringLiteral("等待读取…"));
        setContentPlaceholder(detailCounters_, QStringLiteral("等待读取…"));
        applyButton_->setEnabled(false);
        openLinkButton_->setEnabled(false);
        updateChampionPickerState();
    }
    if (snapshot_.gameId != 0 && snapshot_.gameId != lockedGameId_) {
        lockedChampionId_ = 0;
        selectedChampionIds_.clear();
        lockedGameId_ = snapshot_.gameId;
        lockedLane_.clear();
        lockedMode_.clear();
        build_ = {};
        buildAttempted_ = false;
        buildTitle_->setText(QStringLiteral("锁定英雄后显示推荐配置"));
        buildDetails_->setText(QStringLiteral("符文、属性碎片和召唤师技能会按当前模式与分路从 OP.GG 获取。"));
        applyButton_->setEnabled(false);
        openLinkButton_->setEnabled(false);
        updateChampionPickerState();
    }
    const int lockedChampion = localLockedChampion(snapshot_);
    if (snapshot_.phase.compare(QStringLiteral("ChampSelect"), Qt::CaseInsensitive) == 0 && lockedChampion > 0) {
        const bool selectionChanged = lockedChampion != lockedChampionId_
            || lane.compare(lockedLane_, Qt::CaseInsensitive) != 0
            || mode.compare(lockedMode_, Qt::CaseInsensitive) != 0;
        handleChampionLocked(lockedChampion, lane, mode, selectionChanged);
    }
    if (isVisible() && (oldMode != mode || oldLane != lane)) refreshRanking();
}

void OpggWindow::showFor(QWidget *owner)
{
    if (owner) owner_ = owner;
    if (!owner_) return;
    if (!owner_->isVisible()) {
        QTimer::singleShot(0, this, [this] {
            if (owner_ && owner_->isVisible()) showFor();
        });
        return;
    }
    setSnapshot(gameFlow_.snapshot());
    if (followOwner_) syncToOwner();
    showNormal();
    raise();
    activateWindow();
    if (lockedChampionId_ > 0 && !build_.isValid() && !buildRequestInFlight_) {
        // Reopening the tool is an explicit retry after a transient OPGG
        // failure; normal game-flow polls do not retry continuously.
        buildAttempted_ = false;
        fetchBuild(lockedChampionId_, lockedLane_, lockedMode_);
    }
    if (ranking_.isEmpty() || rankingMode_ != selectedMode()
        || rankingLane_ != selectedLane() || rankingTier_ != selectedRankTier()
        || rankingVersion_ != selectedVersion()) refreshRanking();
}

void OpggWindow::syncToOwner()
{
    if (!followOwner_ || !owner_ || !owner_->isVisible()) return;
    const QRect ownerRect = owner_->frameGeometry();
    const int gap = 10;
    const int availableHeight = owner_->screen() ? owner_->screen()->availableGeometry().height() - 16 : height();
    const int targetHeight = qBound(560, ownerRect.height(), qMax(560, availableHeight));
    if (height() != targetHeight) resize(width(), targetHeight);
    const QRect screenRect = owner_->screen() ? owner_->screen()->availableGeometry() : QRect(ownerRect.topLeft(), QSize(1920, 1080));
    int x = ownerRect.right() + gap;
    if (x + width() > screenRect.right()) x = ownerRect.left() - width() - gap;
    x = qBound(screenRect.left(), x, screenRect.right() - width());
    int y = qBound(screenRect.top(), ownerRect.top(), screenRect.bottom() - height());
    move(x, y);
}

void OpggWindow::setFollowOwner(const bool follow)
{
    followOwner_ = follow;
    if (followOwner_) {
        if (followButton_) {
            QSignalBlocker blocker(followButton_);
            followButton_->setChecked(true);
            followButton_->setText(QStringLiteral("跟随主窗口"));
            followButton_->setToolTip(QStringLiteral("关闭后可独立移动 OP.GG 窗口"));
        }
        syncToOwner();
        status_->setText(QStringLiteral("OP.GG 窗口已恢复跟随主窗口"));
    } else {
        if (followButton_) {
            QSignalBlocker blocker(followButton_);
            followButton_->setChecked(false);
            followButton_->setText(QStringLiteral("独立窗口"));
            followButton_->setToolTip(QStringLiteral("开启后窗口会自动贴在主窗口右侧"));
        }
        status_->setText(QStringLiteral("OP.GG 窗口已独立，可自由移动"));
    }
}

void OpggWindow::refreshRanking()
{
    if (rankingRequestInFlight_) return;
    rankingRequestInFlight_ = true;
    refreshButton_->setEnabled(false);
    refreshButton_->setText(QStringLiteral("读取中…"));
    const QString mode = selectedMode();
    const QString lane = selectedLane();
    const QString requestLane = lane == QStringLiteral("default") ? QString{} : lane;
    const QString rankTier = selectedRankTier();
    const QString version = selectedVersion();
    if (!opggModeSupported(mode)) {
        rankingRequestInFlight_ = false;
        refreshButton_->setEnabled(true);
        refreshButton_->setText(QStringLiteral("刷新"));
        ranking_.clear();
        resetModeChampionPool(mode);
        renderRanking(ranking_);
        updateChampionPickerState();
        status_->setText(QStringLiteral("当前游戏模式没有可用的 OP.GG 英雄数据，已使用默认设置。"));
        return;
    }
    const bool filterChanged = rankingMode_.compare(mode, Qt::CaseInsensitive) != 0
        || rankingLane_.compare(lane, Qt::CaseInsensitive) != 0
        || rankingTier_.compare(rankTier, Qt::CaseInsensitive) != 0
        || rankingVersion_.compare(version, Qt::CaseInsensitive) != 0;
    if (filterChanged) {
        ranking_.clear();
        resetModeChampionPool(mode);
        renderRanking(ranking_);
        updateChampionPickerState();
    }
    rankingMode_ = mode;
    rankingLane_ = lane;
    rankingTier_ = rankTier;
    rankingVersion_ = version;
    status_->setText(QStringLiteral("正在读取韩服 OP.GG · %1 · %2…").arg(modeLabel(mode), AssistController::laneLabel(lane)));
    const QPointer<OpggWindow> window(this);
    opgg_.fetchChampionRanking(mode, requestLane, [window, mode, lane, rankTier, version](QList<OpggChampionRank> ranking, QString error) {
        if (!window) return;
        window->rankingRequestInFlight_ = false;
        window->refreshButton_->setEnabled(true);
        window->refreshButton_->setText(QStringLiteral("刷新"));
        const QString currentMode = window->selectedMode();
        const QString currentLane = window->selectedLane();
        const QString currentRankTier = window->selectedRankTier();
        const QString currentVersion = window->selectedVersion();
        if (currentMode.compare(mode, Qt::CaseInsensitive) != 0
            || currentLane.compare(lane, Qt::CaseInsensitive) != 0
            || currentRankTier.compare(rankTier, Qt::CaseInsensitive) != 0
            || currentVersion.compare(version, Qt::CaseInsensitive) != 0) {
            window->refreshRanking();
            return;
        }
        if (!error.isEmpty()) {
            window->status_->setText(QStringLiteral("OP.GG 读取失败：") + error);
            return;
        }
        window->ranking_ = std::move(ranking);
        for (const OpggChampionRank &entry : window->ranking_) {
            if (!entry.version.trimmed().isEmpty()) {
                window->updateDetectedVersion(entry.version);
                break;
            }
        }
        window->updateModeChampionPool();
        window->renderRanking(window->ranking_);
        window->status_->setText(QStringLiteral("已更新韩服数据 · %1 · %2").arg(modeLabel(mode), AssistController::laneLabel(lane)));
    }, rankTier, version);
}

void OpggWindow::renderRanking(const QList<OpggChampionRank> &ranking)
{
    rankingTable_->setRowCount(0);
    QList<const OpggChampionRank *> filtered;
    filtered.reserve(ranking.size());
    for (const OpggChampionRank &entry : ranking) {
        filtered.append(&entry);
    }
    QSet<int> selectedIds;
    for (const int id : selectedChampionIds_) if (id > 0) selectedIds.insert(id);
    if (selectedIds.isEmpty() && lockedChampionId_ > 0) selectedIds.insert(lockedChampionId_);

    // Keep the normal leaderboard compact while always retaining every
    // manually selected champion. A selected champion outside the top 50 is
    // otherwise easy to mistake for a missing comparison result.
    QList<const OpggChampionRank *> visible;
    visible.reserve(qMin(50, filtered.size()) + selectedIds.size());
    for (int index = 0; index < qMin(50, filtered.size()); ++index) visible.append(filtered.at(index));
    for (const OpggChampionRank *entry : filtered) {
        if (!entry || !selectedIds.contains(championIdForRank(champions_, *entry)) || visible.contains(entry)) continue;
        visible.append(entry);
    }

    rankingTable_->setRowCount(visible.size());
    int selectedRow = -1;
    for (int row = 0; row < visible.size(); ++row) {
        const OpggChampionRank &entry = *visible.at(row);
        const int championId = championIdForRank(champions_, entry);
        const QString name = championName(champions_, championId, entry.championName);
        rankingTable_->setItem(row, 0, new QTableWidgetItem(QString::number(entry.rank > 0 ? entry.rank : row + 1)));
        // The leaderboard is intentionally icon-first and compact.  Keep the
        // localized name in the tooltip so the table remains identifiable
        // without adding a wide text column.
        auto *championItem = new QTableWidgetItem;
        championItem->setData(Qt::UserRole, championId);
        championItem->setToolTip(name);
        championItem->setTextAlignment(Qt::AlignCenter);
        if (championId > 0) {
            const QPixmap portrait = champions_.portraitFor(championId);
            if (!portrait.isNull()) championItem->setIcon(QIcon(portrait));
            else championItem->setText(QStringLiteral("?"));
        } else {
            championItem->setText(QStringLiteral("?"));
        }
        rankingTable_->setItem(row, 1, championItem);
        auto *tierItem = new QTableWidgetItem(displayTier(entry.tier));
        tierItem->setTextAlignment(Qt::AlignCenter);
        rankingTable_->setItem(row, 2, tierItem);
        auto *positionItem = new QTableWidgetItem(AssistController::laneLabel(entry.position));
        positionItem->setTextAlignment(Qt::AlignCenter);
        rankingTable_->setItem(row, 3, positionItem);
        auto *winItem = new QTableWidgetItem(pct(entry.winRate));
        winItem->setTextAlignment(Qt::AlignCenter);
        rankingTable_->setItem(row, 4, winItem);
        auto *pickItem = new QTableWidgetItem(pct(entry.pickRate));
        pickItem->setTextAlignment(Qt::AlignCenter);
        rankingTable_->setItem(row, 5, pickItem);
        auto *banItem = new QTableWidgetItem(pct(entry.banRate));
        banItem->setTextAlignment(Qt::AlignCenter);
        const QColor accent = qApp->property("jannaAccent").value<QColor>();
        if (accent.isValid()) banItem->setForeground(accent);
        rankingTable_->setItem(row, 6, banItem);

        auto *counter = new QWidget(rankingTable_);
        counter->setObjectName(QStringLiteral("opggRankingCounter"));
        counter->setFixedSize(124, 38);
        auto *counterLayout = new QHBoxLayout(counter);
        counterLayout->setContentsMargins(1, 1, 1, 1);
        counterLayout->setSpacing(3);
        QList<OpggChampionReference> counterReferences = entry.weakAgainst;
        for (const OpggChampionReference &reference : entry.strongAgainst) {
            if (counterReferences.size() >= 3) break;
            const bool duplicate = std::any_of(counterReferences.cbegin(), counterReferences.cend(),
                                               [&reference](const OpggChampionReference &current) {
                return reference.championId > 0 && current.championId == reference.championId;
            });
            if (!duplicate) counterReferences.append(reference);
        }
        const int counterCount = qMin(3, counterReferences.size());
        QStringList counterNames;
        for (int counterIndex = 0; counterIndex < counterCount; ++counterIndex) {
            const OpggChampionReference &reference = counterReferences.at(counterIndex);
            const int counterId = champions_.resolveChampionId(reference.championId,
                !reference.championKey.trimmed().isEmpty() ? reference.championKey : reference.championName);
            const QString counterName = championName(champions_, counterId, reference.championName);
            counterNames.append(counterName);
            auto *portrait = new QLabel(counter);
            portrait->setAlignment(Qt::AlignCenter);
            portrait->setFixedSize(34, 34);
            portrait->setToolTip(QStringLiteral("劣势对抗：%1 · 当前英雄胜率 %2")
                .arg(counterName, pct(reference.championWinRate)));
            const QPixmap counterPixmap = counterId > 0 ? champions_.portraitFor(counterId) : QPixmap{};
            if (!counterPixmap.isNull()) portrait->setPixmap(counterPixmap.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            else portrait->setText(QStringLiteral("?"));
            counterLayout->addWidget(portrait);
        }
        if (counterCount == 0) {
            auto *empty = new QLabel(QStringLiteral("--"), counter);
            empty->setAlignment(Qt::AlignCenter);
            counterLayout->addWidget(empty);
        }
        counter->setToolTip(counterNames.isEmpty() ? QStringLiteral("暂无劣势对抗数据")
                                                    : QStringLiteral("劣势对抗：%1").arg(counterNames.join(QStringLiteral("、"))));
        rankingTable_->setCellWidget(row, 7, counter);
        if (selectedIds.contains(championId)) {
            selectedRow = selectedRow < 0 ? row : selectedRow;
            const QColor highlight(QStringLiteral("#E8F4EC"));
            for (int columnIndex = 0; columnIndex < rankingTable_->columnCount(); ++columnIndex) {
                if (QTableWidgetItem *cell = rankingTable_->item(row, columnIndex)) {
                    cell->setBackground(highlight);
                    QFont font = cell->font();
                    font.setBold(true);
                    cell->setFont(font);
                }
            }
        }
    }
    rankingTable_->resizeColumnsToContents();
    rankingTable_->setColumnWidth(1, 64);
    rankingTable_->setColumnWidth(7, 72);
    if (selectedRow >= 0) rankingTable_->setCurrentCell(selectedRow, 1);
    renderSelectedRanking();
}

void OpggWindow::renderSelectedRanking()
{
    if (!selectedRankingLabel_) return;
    QList<int> ids = selectedChampionIds_;
    if (ids.isEmpty() && lockedChampionId_ > 0) ids.append(lockedChampionId_);
    if (ids.isEmpty()) {
        selectedRankingLabel_->setText(QStringLiteral("尚未选择英雄；点击“选择英雄”查看该英雄在当前筛选条件下的排行。"));
        return;
    }

    const QString mode = selectedMode();
    const QString lane = selectedLane();
    const QString tier = rankCombo_ && rankCombo_->currentIndex() >= 0
        ? rankCombo_->currentText() : QStringLiteral("当前段位");
    const QString version = versionCombo_ && versionCombo_->currentIndex() >= 0
        ? versionCombo_->currentText() : QStringLiteral("当前版本");
    const QString context = QStringLiteral("%1 · %2 · %3 · %4")
        .arg(modeLabel(mode), AssistController::laneLabel(lane), tier, version);
    QStringList lines;
    for (const int id : ids) {
        if (id <= 0) continue;
        const OpggChampionRank *selected = nullptr;
        int selectedIndex = -1;
        const QString selectedKey = championKey(champions_.championKeyFor(id));
        for (int index = 0; index < ranking_.size(); ++index) {
            const OpggChampionRank &entry = ranking_.at(index);
            if (championIdForRank(champions_, entry) == id) {
                selected = &entry;
                selectedIndex = index;
                break;
            }
            const QString entryKey = championKey(!entry.championKey.trimmed().isEmpty()
                ? entry.championKey : entry.championName);
            if (!selectedKey.isEmpty() && !entryKey.isEmpty() && entryKey == selectedKey) {
                selected = &entry;
                selectedIndex = index;
                break;
            }
        }
        const QString name = championName(champions_, id);
        if (!selected) {
            lines.append(QStringLiteral("%1 · %2 · 当前筛选条件下暂无排行数据")
                .arg(name, championTypeText(champions_, id)));
            continue;
        }
        const int rank = selected->rank > 0 ? selected->rank : selectedIndex + 1;
        const QString typeText = opggTypeText(champions_, id, selected->roles);
        lines.append(QStringLiteral("%1 · %2 · 第 %3 名 · %4 · 胜率 %5 · 登场 %6 · 禁用 %7")
            .arg(name, typeText, rank > 0 ? QString::number(rank) : QStringLiteral("-"),
                 displayTier(selected->tier), pct(selected->winRate), pct(selected->pickRate), pct(selected->banRate)));
    }
    selectedRankingLabel_->setText(QStringLiteral("已选英雄排行（%1 个）\n%2\n筛选：%3")
        .arg(lines.size()).arg(lines.join(QStringLiteral("\n")), context));
}

void OpggWindow::handleChampionLocked(const int championId, const QString &lane, const QString &mode,
                                      const bool revealWindow)
{
    if (championId <= 0) return;
    const QString normalizedLane = AssistController::normalizeLane(lane).isEmpty()
        ? QStringLiteral("default") : AssistController::normalizeLane(lane);
    const QString normalizedMode = mode.trimmed().isEmpty() ? AssistController::modeKey(snapshot_) : mode.trimmed();
    const bool sameSelection = championId == lockedChampionId_ && normalizedLane == lockedLane_
        && normalizedMode.compare(lockedMode_, Qt::CaseInsensitive) == 0
        && (snapshot_.gameId == 0 || lockedGameId_ == 0 || snapshot_.gameId == lockedGameId_);
    if (sameSelection && !build_.isValid() && !buildRequestInFlight_ && !buildAttempted_) {
        fetchBuild(championId, normalizedLane, normalizedMode);
    }
    if (sameSelection) {
        if (revealWindow) {
            showFor(owner_);
            showDetailPage();
        }
        return;
    }
    lockedChampionId_ = championId;
    selectedChampionIds_.clear();
    selectedChampionIds_.append(championId);
    lockedLane_ = normalizedLane;
    lockedMode_ = normalizedMode;
    updateChampionPickerState();
    if (snapshot_.gameId != 0) lockedGameId_ = snapshot_.gameId;
    populateDetailLaneOptions(championId, normalizedMode, normalizedLane);
    build_ = {};
    buildAttempted_ = false;
    applyButton_->setEnabled(false);
    openLinkButton_->setEnabled(false);
    buildTitle_->setText(QStringLiteral("%1 · 正在读取 OP.GG 推荐配置").arg(championName(champions_, championId)));
    buildDetails_->setText(QStringLiteral("模式：%1 · 分路：%2").arg(modeLabel(normalizedMode), AssistController::laneLabel(normalizedLane)));
    setContentPlaceholder(detailRunes_, QStringLiteral("等待读取…"));
    setContentPlaceholder(detailSkills_, QStringLiteral("等待读取…"));
    setContentPlaceholder(detailItems_, QStringLiteral("等待读取…"));
    setContentPlaceholder(detailCounters_, QStringLiteral("等待读取…"));
    if (revealWindow) {
        showFor(owner_);
        showDetailPage();
    }
    fetchBuild(championId, normalizedLane, normalizedMode);
}

void OpggWindow::fetchBuild(const int championId, const QString &lane, const QString &mode)
{
    if (buildRequestInFlight_) return;
    buildRequestInFlight_ = true;
    buildAttempted_ = true;
    const QPointer<OpggWindow> window(this);
    const QString requestedLane = lane == QStringLiteral("default") ? QString{} : lane;
    const QString rankTier = selectedRankTier();
    const QString version = selectedVersion();
    opgg_.fetchChampionBuild(championId, mode, requestedLane,
                             [window, championId, lane, mode, rankTier, version](OpggBuild build, QString error) {
        if (!window) return;
        window->buildRequestInFlight_ = false;
        if (window->lockedChampionId_ != championId
            || window->lockedLane_.compare(lane, Qt::CaseInsensitive) != 0
            || window->lockedMode_.compare(mode, Qt::CaseInsensitive) != 0
            || window->selectedRankTier().compare(rankTier, Qt::CaseInsensitive) != 0
            || window->selectedVersion().compare(version, Qt::CaseInsensitive) != 0) {
            if (window->lockedChampionId_ > 0) {
                window->fetchBuild(window->lockedChampionId_, window->lockedLane_, window->lockedMode_);
            }
            return;
        }
        if (!error.isEmpty()) {
            window->buildTitle_->setText(QStringLiteral("OP.GG 推荐配置读取失败"));
            window->buildDetails_->setText(error);
            window->status_->setText(error);
            window->openLinkButton_->setEnabled(false);
            emit window->buildFailed(error);
            return;
        }
        if (build.championId <= 0 || build.championId >= 2000) build.championId = championId;
        if (build.championKey.isEmpty()) build.championKey = window->champions_.championKeyFor(build.championId);
        window->updateDetectedVersion(build.version);
        window->build_ = std::move(build);
        window->renderBuild(window->build_);
        emit window->buildReady(window->build_);
    }, championSlug(champions_, championId), rankTier, version);
}

void OpggWindow::setContentPlaceholder(QWidget *content, const QString &text)
{
    if (!content) return;
    clearContentLayout(content);
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    if (!layout) {
        layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    auto *label = new QLabel(text, content);
    label->setObjectName(QStringLiteral("opggDetails"));
    label->setWordWrap(true);
    layout->addWidget(label);
}

void OpggWindow::renderRuneContent(const OpggBuild &build)
{
    if (runeRenderInProgress_) return;
    QScopedValueRollback<bool> renderGuard(runeRenderInProgress_, true);
    clearContentLayout(detailRunes_);
    auto *layout = qobject_cast<QVBoxLayout *>(detailRunes_->layout());
    if (!layout) {
        layout = new QVBoxLayout(detailRunes_);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    layout->setContentsMargins(0, 0, 0, 0);

    const OpggRuneBuild *variant = nullptr;
    for (const OpggRuneBuild &candidate : build.runeBuilds) {
        if (candidate.primaryStyleId == build.primaryStyleId
            && candidate.subStyleId == build.subStyleId
            && candidate.runeIds == build.runeIds
            && candidate.statShardIds == build.statShardIds) {
            variant = &candidate;
            break;
        }
    }
    if (!variant && !build.runeBuilds.isEmpty()) variant = &build.runeBuilds.constFirst();

    QSet<int> selectedRunes;
    for (const int id : build.runeIds) {
        if (id > 0 && !statShardId(id) && id != build.primaryStyleId && id != build.subStyleId) {
            selectedRunes.insert(id);
        }
    }
    QSet<int> selectedShards;
    for (const int id : build.statShardIds) if (statShardId(id)) selectedShards.insert(id);
    if (selectedShards.isEmpty()) {
        for (const int id : build.runeIds) if (statShardId(id)) selectedShards.insert(id);
    }

    QList<QList<int>> primaryRows = variant ? variant->primaryRows : QList<QList<int>>{};
    QList<QList<int>> subRows = variant ? variant->subRows : QList<QList<int>>{};
    QList<QList<int>> shardRows = variant ? variant->statRows : QList<QList<int>>{};
    const QList<QList<OpggRuneChoice>> primaryChoices = variant ? variant->primaryChoices : QList<QList<OpggRuneChoice>>{};
    const QList<QList<OpggRuneChoice>> subChoices = variant ? variant->subChoices : QList<QList<OpggRuneChoice>>{};
    const QList<QList<OpggRuneChoice>> shardChoices = variant ? variant->statChoices : QList<QList<OpggRuneChoice>>{};
    if (primaryRows.isEmpty()) {
        for (const RuneRowDefinition &row : runeRowsForStyle(build.primaryStyleId)) primaryRows.append(row.ids);
    }
    if (subRows.isEmpty()) {
        for (const RuneRowDefinition &row : runeRowsForStyle(build.subStyleId)) subRows.append(row.ids);
    }
    if (shardRows.isEmpty()) {
        for (const RuneRowDefinition &row : statShardRows()) shardRows.append(row.ids);
    }
    QList<QList<int>> subRowsForDisplay = subRows;
    QList<QList<OpggRuneChoice>> subChoicesForDisplay = subChoices;
    // Current OP.GG payloads omit the secondary tree's keystone and expose
    // only its three selectable minor rows. Put the omitted keystone back as
    // a disabled visual row so the three real rows remain selectable.
    if (subRowsForDisplay.size() == 3) {
        const QList<RuneRowDefinition> fallbackRows = runeRowsForStyle(build.subStyleId);
        if (!fallbackRows.isEmpty()) {
            subRowsForDisplay.prepend(fallbackRows.constFirst().ids);
            subChoicesForDisplay.prepend({});
        }
    }

    // A local LCU/CommunityDragon icon is preferred, but it is not always
    // available (especially for a newly introduced or renamed rune).  Keep
    // the OP.GG URL next to the current rows and fill the cache asynchronously.
    runeImageUrls_.clear();
    QString fallbackVersion = build.version.trimmed();
    if (fallbackVersion.isEmpty()) fallbackVersion = detectedVersion_.trimmed();
    if (fallbackVersion.isEmpty()) fallbackVersion = selectedVersion();
    const auto requestRuneImage = [this](const int id, const QString &rawUrl) {
        if (id <= 0) return;
        const QString url = normalizedRuneImageUrl(rawUrl);
        if (url.isEmpty()) return;
        runeImageUrls_.insert(id, url);
        const QUrl parsedUrl(url);
        if (!parsedUrl.isValid()
            || parsedUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
            || parsedUrl.host().isEmpty() || parsedUrl.path().isEmpty()) {
            runeImageFailures_.insert(url);
            return;
        }
        if (!champions_.runeIconFor(id).isNull()
            || runeImageCache_.contains(url)
            || runeImageRequests_.contains(url)
            || runeImageFailures_.contains(url)) {
            return;
        }
        runeImageRequests_.insert(url);
        const QPointer<OpggWindow> window(this);
        opgg_.fetchImage(url, [window, url](QByteArray bytes, QString error) {
            if (!window) return;
            window->runeImageRequests_.remove(url);
            QImage image;
            bool loaded = false;
            if (error.isEmpty() && !bytes.isEmpty() && image.loadFromData(bytes)) {
                window->runeImageCache_.insert(url, QPixmap::fromImage(
                    image.convertToFormat(QImage::Format_ARGB32_Premultiplied)));
                window->runeImageFailures_.remove(url);
                loaded = true;
            } else {
                // Do not start a tight retry/render loop when a public asset
                // is unavailable. A later build refresh can clear this set.
                window->runeImageFailures_.insert(url);
            }
            // A failed image does not change the visible rune layout.  Only a
            // successful asset load needs a refresh, and coalesce several
            // network completions into one queued render so a callback cannot
            // invalidate a layout while it is still being activated.
            if (loaded && window->runeImageUrls_.values().contains(url)
                && window->build_.isValid() && !window->runeRefreshQueued_) {
                window->runeRefreshQueued_ = true;
                const QPointer<OpggWindow> target(window);
                QTimer::singleShot(0, window.data(), [target] {
                    if (!target) return;
                    target->runeRefreshQueued_ = false;
                    if (target->build_.isValid()) target->renderRuneContent(target->build_);
                });
            }
        });
    };
    const auto scheduleRuneImages = [this, &requestRuneImage, &fallbackVersion](
                                        const QList<QList<OpggRuneChoice>> &rows) {
        for (const QList<OpggRuneChoice> &row : rows) {
            for (const OpggRuneChoice &choice : row) {
                if (choice.id <= 0) continue;
                QString url = normalizedRuneImageUrl(choice.imageUrl);
                if (url.isEmpty()) url = fallbackRuneImageUrl(choice.id, fallbackVersion);
                requestRuneImage(choice.id, url);
            }
        }
    };
    scheduleRuneImages(primaryChoices);
    scheduleRuneImages(subChoices);
    scheduleRuneImages(shardChoices);
    const auto scheduleFallbackRows = [this, &requestRuneImage, &fallbackVersion](const QList<QList<int>> &rows) {
        for (const QList<int> &row : rows) {
            for (const int id : row) {
                if (id > 0 && !runeImageUrls_.contains(id)) {
                    requestRuneImage(id, fallbackRuneImageUrl(id, fallbackVersion));
                }
            }
        }
    };
    // Hard-coded catalogue rows are used when a payload omits structured
    // choices. Give those ids the same OP.GG CDN fallback as structured rows.
    scheduleFallbackRows(primaryRows);
    scheduleFallbackRows(subRowsForDisplay);
    scheduleFallbackRows(shardRows);
    const auto runePixmapFor = [this](const int id, const QString &explicitUrl) {
        const QPixmap local = champions_.runeIconFor(id);
        if (!local.isNull()) return local;
        const QString normalizedUrl = normalizedRuneImageUrl(explicitUrl);
        const QString url = normalizedUrl.isEmpty() ? runeImageUrls_.value(id) : normalizedUrl;
        return url.isEmpty() ? QPixmap{} : runeImageCache_.value(url);
    };

    QSet<int> knownRuneIds;
    auto addTree = [&](const QString &title, const int styleId, const bool primary,
                       const QList<QList<int>> &rows,
                       const QList<QList<OpggRuneChoice>> &choices) {
        if (styleId <= 0 || rows.isEmpty()) {
            auto *missing = new QLabel(title + QStringLiteral("：未提供"), detailRunes_);
            missing->setObjectName(QStringLiteral("opggDetails"));
            layout->addWidget(missing);
            return;
        }
        layout->addWidget(sectionLabel(detailRunes_, title + QStringLiteral(" · ") + runeStyleName(styleId)));
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const QList<int> &ids = rows.at(rowIndex);
            auto *rowHost = new QWidget(detailRunes_);
            auto *rowLayout = new QVBoxLayout(rowHost);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(2);
            QString rowText = QStringLiteral("第 %1 行").arg(rowIndex + 1);
            if (!primary && rowIndex == 0) rowText = QStringLiteral("基石（不可选）");
            auto *rowLabel = new QLabel(rowText, rowHost);
            rowLabel->setObjectName(QStringLiteral("opggRuneRowLabel"));
            rowLayout->addWidget(rowLabel);
            auto *grid = new QGridLayout;
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(5);
            grid->setVerticalSpacing(4);
            const int columns = ids.size() >= 4 ? 4 : 3;
            bool rowSelected = false;
            for (int index = 0; index < ids.size(); ++index) {
                const int id = ids.at(index);
                QString name;
                QString imageUrl;
                if (rowIndex < choices.size() && index < choices.at(rowIndex).size()) {
                    name = choices.at(rowIndex).at(index).name;
                    imageUrl = choices.at(rowIndex).at(index).imageUrl;
                }
                knownRuneIds.insert(id);
                const bool selected = (primary || rowIndex > 0) && selectedRunes.contains(id) && !rowSelected;
                if (selected) rowSelected = true;
                grid->addWidget(runeOptionChip(rowHost, name.isEmpty() ? runeDisplayName(champions_, id) : name,
                                                runePixmapFor(id, imageUrl), 34, selected,
                                                primary || rowIndex > 0), index / columns, index % columns);
            }
            rowLayout->addLayout(grid);
            layout->addWidget(rowHost);
        }
    };

    addTree(QStringLiteral("主系"), build.primaryStyleId, true, primaryRows, primaryChoices);
    addTree(QStringLiteral("副系"), build.subStyleId, false, subRowsForDisplay, subChoicesForDisplay);

    QList<int> unknown;
    for (const int id : selectedRunes) if (!knownRuneIds.contains(id)) unknown.append(id);
    if (!unknown.isEmpty()) {
        layout->addWidget(sectionLabel(detailRunes_, QStringLiteral("其他已选符文")));
        auto *gridHost = new QWidget(detailRunes_);
        auto *grid = new QGridLayout(gridHost);
        grid->setContentsMargins(0, 0, 0, 0);
        for (int index = 0; index < unknown.size(); ++index) {
            const int id = unknown.at(index);
            grid->addWidget(runeOptionChip(gridHost, runeDisplayName(champions_, id),
                                            runePixmapFor(id, {}), 34, true), index / 4, index % 4);
        }
        layout->addWidget(gridHost);
    }

    layout->addWidget(sectionLabel(detailRunes_, QStringLiteral("属性碎片")));
    for (int rowIndex = 0; rowIndex < shardRows.size(); ++rowIndex) {
        const QList<int> &ids = shardRows.at(rowIndex);
        auto *rowHost = new QWidget(detailRunes_);
        auto *rowLayout = new QVBoxLayout(rowHost);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto *rowLabel = new QLabel(QStringLiteral("第 %1 行").arg(rowIndex + 1), rowHost);
        rowLabel->setObjectName(QStringLiteral("opggRuneRowLabel"));
        rowLayout->addWidget(rowLabel);
        auto *grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        bool rowSelected = false;
        for (int index = 0; index < ids.size(); ++index) {
            const int id = ids.at(index);
            const bool selected = selectedShards.contains(id) && !rowSelected;
            if (selected) rowSelected = true;
            QString name;
            QString imageUrl;
            if (rowIndex < shardChoices.size() && index < shardChoices.at(rowIndex).size()) {
                name = shardChoices.at(rowIndex).at(index).name;
                imageUrl = shardChoices.at(rowIndex).at(index).imageUrl;
            }
            grid->addWidget(runeOptionChip(rowHost, name.isEmpty() ? runeDisplayName(champions_, id) : name,
                                            runePixmapFor(id, imageUrl), 30, selected), index / 5, index % 5);
        }
        rowLayout->addLayout(grid);
        layout->addWidget(rowHost);
    }

    layout->addWidget(sectionLabel(detailRunes_, QStringLiteral("推荐召唤师技能")));
    QList<OpggSpellBuild> spellBuilds = build.spellBuilds;
    if (spellBuilds.isEmpty() && build.summonerSpellIds.size() >= 2) {
        OpggSpellBuild fallback;
        fallback.spellIds = build.summonerSpellIds.mid(0, 2);
        spellBuilds.append(std::move(fallback));
    }
    if (spellBuilds.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("未提供"), detailRunes_);
        empty->setObjectName(QStringLiteral("opggDetails"));
        layout->addWidget(empty);
    } else {
        for (const OpggSpellBuild &spellBuild : spellBuilds) {
            auto *gridHost = new QWidget(detailRunes_);
            auto *grid = new QHBoxLayout(gridHost);
            grid->setContentsMargins(0, 0, 0, 0);
            for (int spellIndex = 0; spellIndex < spellBuild.spellIds.size(); ++spellIndex) {
                const int id = spellBuild.spellIds.at(spellIndex);
                const QString opggName = spellIndex < spellBuild.spellNames.size()
                    ? spellBuild.spellNames.at(spellIndex).trimmed() : QString{};
                const QString spellName = opggName.isEmpty() ? spellDisplayName(champions_, id) : opggName;
                grid->addWidget(assetChip(gridHost, spellName, champions_.summonerSpellIconFor(id), 36, spellName));
            }
            if (spellBuild.pickRate > 0.0) {
                auto *rate = new QLabel(QStringLiteral("选取 %1").arg(pct(spellBuild.pickRate)), gridHost);
                rate->setObjectName(QStringLiteral("opggDetails"));
                grid->addWidget(rate);
            }
            grid->addStretch();
            layout->addWidget(gridHost);
        }
    }

    // Widgets created by a queued image refresh can be added after the detail
    // page is already visible.  Explicitly show the new descendants and
    // activate the layout so Qt does not leave them at (0, 0) with a zero
    // size hint after the old contents were deleted.
    detailRunes_->setVisible(true);
    for (QWidget *widget : detailRunes_->findChildren<QWidget *>()) widget->show();
    layout->activate();
    detailRunes_->updateGeometry();
}

void OpggWindow::renderSkillContent(const OpggBuild &build)
{
    clearContentLayout(detailSkills_);
    auto *layout = qobject_cast<QVBoxLayout *>(detailSkills_->layout());
    if (!layout) {
        layout = new QVBoxLayout(detailSkills_);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    layout->setContentsMargins(0, 0, 0, 0);
    if (build.skillOrder.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("OP.GG 未提供技能加点顺序"), detailSkills_);
        empty->setObjectName(QStringLiteral("opggDetails"));
        layout->addWidget(empty);
        return;
    }

    auto *sequence = new QWidget(detailSkills_);
    auto *sequenceLayout = new QHBoxLayout(sequence);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    sequenceLayout->setSpacing(3);
    for (int index = 0; index < build.skillOrder.size(); ++index) {
        auto *host = new QWidget(sequence);
        auto *hostLayout = new QVBoxLayout(host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(1);
        auto *badge = new QLabel(build.skillOrder.at(index), host);
        badge->setObjectName(QStringLiteral("opggSkillBadge"));
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(27, 27);
        hostLayout->addWidget(badge, 0, Qt::AlignHCenter);
        auto *level = new QLabel(QString::number(index + 1), host);
        level->setObjectName(QStringLiteral("opggSkillLevel"));
        level->setAlignment(Qt::AlignCenter);
        hostLayout->addWidget(level);
        sequenceLayout->addWidget(host);
    }
    sequenceLayout->addStretch();
    layout->addWidget(sequence);
}

void OpggWindow::renderItemContent(const OpggBuild &build)
{
    clearContentLayout(detailItems_);
    auto *layout = qobject_cast<QVBoxLayout *>(detailItems_->layout());
    if (!layout) {
        layout = new QVBoxLayout(detailItems_);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    layout->setContentsMargins(0, 0, 0, 0);
    const auto addStage = [this, layout](const QString &title,
                                         const QList<OpggItemBuild::Group> &groups,
                                         const QList<int> &legacyIds) {
        if (groups.isEmpty() && legacyIds.isEmpty()) return;
        layout->addWidget(sectionLabel(detailItems_, title));
        if (!groups.isEmpty()) {
            for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                const OpggItemBuild::Group &group = groups.at(groupIndex);
                auto *rowHost = new QFrame(detailItems_);
                rowHost->setObjectName(QStringLiteral("opggItemGroup"));
                auto *rowLayout = new QHBoxLayout(rowHost);
                rowLayout->setContentsMargins(4, 3, 4, 3);
                rowLayout->setSpacing(6);
                auto *groupLabel = new QLabel(group.label.isEmpty() ? QStringLiteral("方案 %1").arg(groupIndex + 1) : group.label,
                                              rowHost);
                groupLabel->setObjectName(QStringLiteral("opggItemGroupLabel"));
                groupLabel->setMinimumWidth(70);
                rowLayout->addWidget(groupLabel);
                for (const OpggItemBuild::Choice &choice : group.choices) {
                    if (!choice.isValid()) continue;
                    const QString name = choice.name.isEmpty() ? itemDisplayName(champions_, choice.itemId) : choice.name;
                    auto *chip = assetChip(rowHost, name, champions_.itemIconFor(choice.itemId), 34, name);
                    rowLayout->addWidget(chip);
                    const double rate = choice.pickRate > 0.0 ? choice.pickRate : group.pickRate;
                    if (rate > 0.0) {
                        auto *rateLabel = new QLabel(QStringLiteral("%1%").arg(QString::number(rate, 'f', 1)), rowHost);
                        rateLabel->setObjectName(QStringLiteral("opggItemRate"));
                        rateLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
                        rowLayout->addWidget(rateLabel);
                    }
                }
                if (group.games > 0) {
                    auto *games = new QLabel(QStringLiteral("%1 场").arg(group.games), rowHost);
                    games->setObjectName(QStringLiteral("opggDetails"));
                    rowLayout->addWidget(games);
                }
                rowLayout->addStretch();
                layout->addWidget(rowHost);
            }
            return;
        }
        auto *host = new QWidget(detailItems_);
        auto *grid = new QGridLayout(host);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(3);
        grid->setVerticalSpacing(4);
        int cell = 0;
        for (const int id : legacyIds) {
            if (id <= 0) continue;
            const QString name = itemDisplayName(champions_, id);
            grid->addWidget(assetChip(host, name, champions_.itemIconFor(id), 36, name), cell / 4, cell % 4);
            ++cell;
        }
        if (cell > 0) layout->addWidget(host);
    };

    const OpggItemBuild *variant = nullptr;
    for (const OpggItemBuild &candidate : build.itemBuilds) {
        if (candidate.itemIds == build.itemIds
            || (candidate.starterItemIds == build.itemIds
                && candidate.bootsItemIds.isEmpty() && candidate.coreItemIds.isEmpty())) {
            variant = &candidate;
            break;
        }
    }
    if (!variant && !build.itemBuilds.isEmpty()) variant = &build.itemBuilds.constFirst();
    const QList<int> starter = variant ? variant->starterItemIds : QList<int>{};
    const QList<int> boots = variant ? variant->bootsItemIds : QList<int>{};
    const QList<int> support = variant ? variant->supportItemIds : QList<int>{};
    const QList<int> core = variant ? variant->coreItemIds : QList<int>{};
    const QList<int> final = variant ? variant->finalItemIds : QList<int>{};
    const auto emptyGroups = QList<OpggItemBuild::Group>{};
    if (variant && (!variant->starterGroups.isEmpty() || !variant->bootsGroups.isEmpty()
                    || !variant->supportGroups.isEmpty() || !variant->coreGroups.isEmpty()
                    || !variant->fourthGroups.isEmpty() || !variant->fifthGroups.isEmpty()
                    || !variant->sixthGroups.isEmpty())) {
        addStage(QStringLiteral("出门装"), variant->starterGroups, starter);
        addStage(QStringLiteral("鞋子"), variant->bootsGroups, boots);
        addStage(QStringLiteral("辅助装"), variant->supportGroups, support);
        addStage(QStringLiteral("核心装备顺序"), variant->coreGroups, core);
        addStage(QStringLiteral("第四件装备"), variant->fourthGroups, {});
        addStage(QStringLiteral("第五件装备"), variant->fifthGroups, {});
        addStage(QStringLiteral("第六件装备"), variant->sixthGroups, {});
    } else if (starter.isEmpty() && boots.isEmpty() && core.isEmpty() && final.isEmpty()) {
        QList<int> items;
        for (const int id : build.itemIds) if (id > 0) items.append(id);
        if (!items.isEmpty()) addStage(QStringLiteral("完整顺序"), emptyGroups, items);
    } else {
        addStage(QStringLiteral("出门装"), emptyGroups, starter);
        addStage(QStringLiteral("鞋子"), emptyGroups, boots);
        addStage(QStringLiteral("辅助装"), emptyGroups, support);
        addStage(QStringLiteral("核心装备顺序"), emptyGroups, core);
        addStage(QStringLiteral("后续装备顺序"), emptyGroups, final);
    }

    const bool hasItems = !starter.isEmpty() || !boots.isEmpty() || !support.isEmpty() || !core.isEmpty() || !final.isEmpty()
        || !build.itemIds.isEmpty();
    if (!hasItems) {
        auto *empty = new QLabel(QStringLiteral("OP.GG 未提供推荐出装"), detailItems_);
        empty->setObjectName(QStringLiteral("opggDetails"));
        layout->addWidget(empty);
    }
}

void OpggWindow::applyRuneVariant(const int index)
{
    if (index < 0 || index >= build_.runeBuilds.size()) return;
    const OpggRuneBuild &variant = build_.runeBuilds.at(index);
    build_.primaryStyleId = variant.primaryStyleId;
    build_.subStyleId = variant.subStyleId;
    build_.runeIds = variant.runeIds;
    build_.statShardIds = variant.statShardIds;
    build_.summonerSpellIds = variant.summonerSpellIds;
    renderRuneContent(build_);
    applyButton_->setEnabled(build_.isValid() && build_.primaryStyleId > 0 && !build_.runeIds.isEmpty());
    status_->setText(QStringLiteral("已切换符文方案：%1").arg(
        variant.label.isEmpty() ? QStringLiteral("方案 %1").arg(index + 1) : variant.label));
    emit buildReady(build_);
}

void OpggWindow::applyItemVariant(const int index)
{
    if (index < 0 || index >= build_.itemBuilds.size()) return;
    const OpggItemBuild &variant = build_.itemBuilds.at(index);
    build_.itemIds = variant.itemIds;
    renderItemContent(build_);
    status_->setText(QStringLiteral("已切换出装方案：%1").arg(
        variant.label.isEmpty() ? QStringLiteral("方案 %1").arg(index + 1) : variant.label));
    emit buildReady(build_);
}

void OpggWindow::renderCounterContent(const OpggBuild &build)
{
    clearContentLayout(detailCounters_);
    auto *layout = qobject_cast<QVBoxLayout *>(detailCounters_->layout());
    if (!layout) {
        layout = new QVBoxLayout(detailCounters_);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    layout->setContentsMargins(0, 0, 0, 0);
    QList<OpggChampionReference> weak = build.weakCounters;
    QList<OpggChampionReference> favorable = build.favorableCounters;
    if (weak.isEmpty() && favorable.isEmpty()) {
        for (const OpggChampionReference &counter : build.counters) {
            const QString relation = counter.relation.toLower();
            if (relation.contains(QStringLiteral("favor")) || relation.contains(QStringLiteral("strong"))
                || relation.contains(QStringLiteral("优势"))) favorable.append(counter);
            else weak.append(counter);
        }
    }
    if (weak.isEmpty() && favorable.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("OP.GG 未提供 Counter 数据"), detailCounters_);
        empty->setObjectName(QStringLiteral("opggDetails"));
        layout->addWidget(empty);
        return;
    }

    const auto addCounterGroup = [this, layout](const QString &title, const QList<OpggChampionReference> &counters,
                                                 const bool weakGroup) {
        if (counters.isEmpty()) return;
        layout->addWidget(sectionLabel(detailCounters_, title));
        auto *gridHost = new QWidget(detailCounters_);
        auto *grid = new QGridLayout(gridHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(6);
        const int count = qMin(5, counters.size());
        for (int index = 0; index < count; ++index) {
            const OpggChampionReference &counter = counters.at(index);
            const int id = champions_.resolveChampionId(counter.championId,
                !counter.championKey.trimmed().isEmpty() ? counter.championKey : counter.championName);
            const QString name = championName(champions_, id, counter.championName);
            auto *card = new QFrame(gridHost);
            card->setObjectName(QStringLiteral("opggCounterCard"));
            auto *cardLayout = new QHBoxLayout(card);
            cardLayout->setContentsMargins(6, 5, 7, 5);
            cardLayout->setSpacing(7);
            auto *portrait = new QLabel(card);
            portrait->setObjectName(QStringLiteral("opggCounterPortrait"));
            portrait->setAlignment(Qt::AlignCenter);
            portrait->setFixedSize(42, 42);
            const QPixmap pixmap = id > 0 ? champions_.portraitFor(id) : QPixmap{};
            if (!pixmap.isNull()) portrait->setPixmap(pixmap.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            else portrait->setText(QStringLiteral("?"));
            cardLayout->addWidget(portrait);
            auto *text = new QVBoxLayout;
            text->setContentsMargins(0, 0, 0, 0);
            text->setSpacing(1);
            auto *nameLabel = new QLabel(name, card);
            nameLabel->setObjectName(QStringLiteral("opggCounterName"));
            text->addWidget(nameLabel);
            auto *relation = new QLabel(weakGroup ? QStringLiteral("对手占优") : QStringLiteral("我方占优"), card);
            relation->setObjectName(weakGroup ? QStringLiteral("opggCounterBad") : QStringLiteral("opggCounterGood"));
            text->addWidget(relation);
            const double selectedRate = counter.championWinRate > 0.0 ? counter.championWinRate : counter.winRate;
            if (selectedRate > 0.0) {
                auto *rate = new QLabel(QStringLiteral("当前英雄胜率 %1").arg(pct(selectedRate)), card);
                rate->setObjectName(QStringLiteral("opggCounterRate"));
                text->addWidget(rate);
            }
            if (counter.games > 0) {
                auto *games = new QLabel(QStringLiteral("%1 场").arg(counter.games), card);
                games->setObjectName(QStringLiteral("opggDetails"));
                text->addWidget(games);
            }
            cardLayout->addLayout(text, 1);
            grid->addWidget(card, index / 2, index % 2);
        }
        layout->addWidget(gridHost);
    };
    addCounterGroup(QStringLiteral("劣势对抗（前五）"), weak, true);
    addCounterGroup(QStringLiteral("优势对抗（前五）"), favorable, false);
}

void OpggWindow::renderBuild(const OpggBuild &build)
{
    const QString name = championName(champions_, build.championId, build.championName);
    // A new OP.GG response may point at a newly published asset URL. Allow a
    // fresh response/refresh to retry URLs that failed for an earlier build.
    runeImageFailures_.clear();
    // Keep the first structured variant available to the legacy apply path
    // even when a caller supplies only the new variant lists.
    if (&build != &build_) build_ = build;
    if (build_.runeBuilds.isEmpty() && build_.primaryStyleId > 0 && !build_.runeIds.isEmpty()) {
        OpggRuneBuild fallback;
        fallback.label = QStringLiteral("方案 1");
        fallback.primaryStyleId = build_.primaryStyleId;
        fallback.subStyleId = build_.subStyleId;
        fallback.runeIds = build_.runeIds;
        fallback.statShardIds = build_.statShardIds;
        fallback.summonerSpellIds = build_.summonerSpellIds;
        build_.runeBuilds.append(std::move(fallback));
    }
    if (build_.itemBuilds.isEmpty() && !build_.itemIds.isEmpty()) {
        OpggItemBuild fallback;
        fallback.label = QStringLiteral("方案 1");
        fallback.itemIds = build_.itemIds;
        build_.itemBuilds.append(std::move(fallback));
    }
    buildTitle_->setText(QStringLiteral("OP.GG 推荐配置 · %1").arg(name));
    QStringList roleLabels = build_.roles;
    if (roleLabels.isEmpty()) roleLabels = champions_.championTypeLabelsFor(build_.championId);
    const QString roleText = roleLabels.isEmpty() ? QStringLiteral("--") : roleLabels.join(QStringLiteral(" / "));
    const QString version = build_.version.isEmpty()
        ? (selectedVersion().isEmpty() ? QStringLiteral("当前版本") : selectedVersion()) : build_.version;
    const QString authoritativeLane = build_.position.trimmed().isEmpty() ? lockedLane_ : build_.position;
    QString details = QStringLiteral("模式：%1 · 当前分路：%2")
        .arg(modeLabel(lockedMode_), AssistController::laneLabel(authoritativeLane));
    buildDetails_->setToolTip(QStringLiteral("OP.GG 版本 %1").arg(version));
    buildDetails_->setText(details);
    detailRole_->setText(QStringLiteral("定位：%1").arg(roleText));
    detailVersion_->setText(QStringLiteral("版本：%1").arg(version));
    QStringList laneParts;
    for (const OpggLaneStat &lane : build_.laneStats) {
        laneParts.append(QStringLiteral("%1 %2").arg(AssistController::laneLabel(lane.position), pct(lane.pickRate)));
    }
    detailLaneStats_->setText(laneParts.isEmpty()
        ? QStringLiteral("分路：%1").arg(AssistController::laneLabel(authoritativeLane))
        : QStringLiteral("分路：%1").arg(laneParts.join(QStringLiteral(" · "))));
    QString rankKey = selectedRankTier();
    rankKey.replace(QStringLiteral("_plus"), QString{});
    if (rankKey == QStringLiteral("all") || rankKey.isEmpty()) rankKey = QStringLiteral("emerald");
    const QPixmap rankPixmap = champions_.rankEmblemFor(rankKey);
    const QString rankLabel = rankCombo_ && rankCombo_->currentIndex() >= 0
        ? rankCombo_->currentText() : QStringLiteral("当前段位");
    if (detailRankText_) detailRankText_->setText(QStringLiteral("段位：%1").arg(rankLabel));
    if (!rankPixmap.isNull()) {
        detailRankIcon_->setPixmap(rankPixmap.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        detailRankIcon_->setToolTip(QStringLiteral("统计段位：%1").arg(rankLabel));
        detailRankIcon_->setText(QString{});
    } else {
        detailRankIcon_->setText(QStringLiteral("段"));
        detailRankIcon_->setToolTip(QStringLiteral("统计段位：%1").arg(rankLabel));
    }
    QSignalBlocker runeBlocker(runeVariantCombo_);
    QSignalBlocker itemBlocker(itemVariantCombo_);
    runeVariantCombo_->clear();
    for (int index = 0; index < build_.runeBuilds.size(); ++index) {
        const OpggRuneBuild &variant = build_.runeBuilds.at(index);
        const QString label = variant.label.isEmpty() ? QStringLiteral("方案 %1").arg(index + 1) : variant.label;
        runeVariantCombo_->addItem(label, index);
    }
    itemVariantCombo_->clear();
    for (int index = 0; index < build_.itemBuilds.size(); ++index) {
        const OpggItemBuild &variant = build_.itemBuilds.at(index);
        const QString label = variant.label.isEmpty() ? QStringLiteral("方案 %1").arg(index + 1) : variant.label;
        itemVariantCombo_->addItem(label, index);
    }
    runeVariantCombo_->setEnabled(build_.runeBuilds.size() > 1);
    itemVariantCombo_->setEnabled(build_.itemBuilds.size() > 1);
    int runeIndex = 0;
    for (int index = 0; index < build_.runeBuilds.size(); ++index) {
        const OpggRuneBuild &variant = build_.runeBuilds.at(index);
        if (variant.primaryStyleId == build_.primaryStyleId && variant.subStyleId == build_.subStyleId
            && variant.runeIds == build_.runeIds && variant.statShardIds == build_.statShardIds) {
            runeIndex = index;
            break;
        }
    }
    int itemIndex = 0;
    for (int index = 0; index < build_.itemBuilds.size(); ++index) {
        if (build_.itemBuilds.at(index).itemIds == build_.itemIds) {
            itemIndex = index;
            break;
        }
    }
    if (runeVariantCombo_->count() > 0) runeVariantCombo_->setCurrentIndex(runeIndex);
    if (itemVariantCombo_->count() > 0) itemVariantCombo_->setCurrentIndex(itemIndex);
    renderRuneContent(build_);
    renderSkillContent(build_);
    renderItemContent(build_);
    renderCounterContent(build_);
    applyButton_->setEnabled(build_.isValid() && build_.primaryStyleId > 0 && !build_.runeIds.isEmpty());
    openLinkButton_->setEnabled(!championSlug(champions_, build_.championId).isEmpty());
    status_->setText(QStringLiteral("已读取 %1 的推荐配置").arg(name));
}

int OpggWindow::localLockedChampion(const GameFlowSnapshot &snapshot) const
{
    for (const GameFlowPlayer &player : snapshot.myTeam) {
        if (player.isLocalPlayer && player.pickCompleted && player.championId > 0) return player.championId;
    }
    return 0;
}

void OpggWindow::updateContextLabels()
{
    const QString mode = selectedMode();
    const QString lane = selectedLane();
    const bool followsSnapshot = !automaticMode_.isEmpty()
        && mode.compare(automaticMode_, Qt::CaseInsensitive) == 0
        && lane.compare(automaticLane_, Qt::CaseInsensitive) == 0;
    context_->setText(QStringLiteral("%1 · %2%3").arg(modeLabel(mode), AssistController::laneLabel(lane),
        followsSnapshot ? QStringLiteral("（对局同步）") : QStringLiteral("（手动筛选）")));
}

void OpggWindow::closeEvent(QCloseEvent *event)
{
    hide();
    event->ignore();
    emit closed();
}

void OpggWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor border = qApp->property("jannaBorder").value<QColor>();
    painter.setBrush(palette().color(QPalette::Window));
    painter.setPen(QPen(border.isValid() ? border : palette().color(QPalette::Mid), 1));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 11, 11);
}

void OpggWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!(windowFlags() & Qt::FramelessWindowHint)) return;
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 11, 11);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

} // namespace Janna
