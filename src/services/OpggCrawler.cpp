#include "services/OpggCrawler.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace Janna {
namespace {

QString encoded(const QString &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value.trimmed()));
}

QString normalizedPosition(const QString &position)
{
    QString value = position.trimmed().toLower();
    // App-router and internal API responses have used all of these spellings
    // over time. Normalize punctuation/whitespace before matching so a
    // nested label such as "bottom-lane" cannot leak into the role field.
    value.remove(QRegularExpression(QStringLiteral("[\\s_\\-]")));
    // OP.GG has emitted both compact lane codes and human-readable route
    // labels over different app-router revisions (for example
    // `bottom-lane`, `mid_lane`, and `support lane`).  Strip the suffix after
    // punctuation has been normalized so all of them resolve to one lane.
    if (value.endsWith(QStringLiteral("lane"))) value.chop(4);
    if (value == "top" || value == QString::fromUtf8("上路")) return "top";
    if (value == "jungle" || value == "jg" || value == QString::fromUtf8("打野")) return "jungle";
    if (value == "mid" || value == "middle" || value == QString::fromUtf8("中路")) return "mid";
    if (value == "bot" || value == "bottom" || value == "adc" || value == QString::fromUtf8("下路")) return "adc";
    if (value == "support" || value == "utility" || value == "sup" || value == QString::fromUtf8("辅助")) return "support";
    return {};
}

QString normalizedMode(const QString &mode)
{
    const QString value = mode.trimmed().toUpper();
    if (value.isEmpty()) return "NORMAL";
    if (value == "RANKED_SOLO_5X5" || value == "SOLORANKED" || value == "SOLO" || value == "RANKED") return "SOLORANKED";
    if (value == "RANKED_FLEX_SR" || value == "FLEXRANKED" || value == "FLEX") return "FLEXRANKED";
    if (value == "ARAM" || value == "ARAM_5X5") return "ARAM";
    if (value == "NORMAL" || value == "NORMAL_5X5" || value == "DRAFT" || value == "CLASSIC") return "NORMAL";
    return value;
}

QString normalizedRankTier(const QString &tier)
{
    const QString value = tier.trimmed().toLower();
    static const QSet<QString> supported = {
        QStringLiteral("all"), QStringLiteral("iron"), QStringLiteral("bronze"), QStringLiteral("silver"),
        QStringLiteral("gold"), QStringLiteral("platinum"), QStringLiteral("emerald"), QStringLiteral("diamond"),
        QStringLiteral("master"), QStringLiteral("grandmaster"), QStringLiteral("challenger"),
        QStringLiteral("iron_plus"), QStringLiteral("bronze_plus"), QStringLiteral("silver_plus"),
        QStringLiteral("gold_plus"), QStringLiteral("platinum_plus"), QStringLiteral("emerald_plus"),
        QStringLiteral("diamond_plus"), QStringLiteral("master_plus")
    };
    return supported.contains(value) ? value : QStringLiteral("emerald_plus");
}

QString normalizedVersion(const QString &version)
{
    const QString value = version.trimmed();
    static const QRegularExpression patchPattern(QStringLiteral("^\\d{1,2}\\.\\d{1,2}(?:\\.\\d+)?$"));
    return patchPattern.match(value).hasMatch() ? value : QString{};
}

QString opggWebType(const QString &mode)
{
    if (mode == "SOLORANKED") return "ranked";
    if (mode == "FLEXRANKED") return "flex";
    if (mode == "NORMAL") return "classic";
    return {};
}

QString opggModeRoute(const QString &mode)
{
    if (mode == "ARENA") return "/lol/modes/arena";
    if (mode == "ONEFORALL") return "/lol/modes/one-for-all";
    if (mode == "ULTBOOK") return "/lol/modes/ultimate-spellbook";
    return {};
}

QString localizedOpggPath(const QString &path)
{
    if (path.startsWith(QStringLiteral("/zh-cn/"))) return path;
    return QStringLiteral("/zh-cn") + path;
}

QString decodeHtmlEntities(QString value);
QString opggRoleCode(QString value);
QHash<QString, QString> localizedRoleLabelsFromMarkup(const QByteArray &payload);
void applyLocalizedRoleLabels(QStringList &roles, const QHash<QString, QString> &labels);
QString lanePositionFromObject(const QJsonObject &object);

bool opggModeSupported(const QString &mode)
{
    return mode == QStringLiteral("SOLORANKED") || mode == QStringLiteral("FLEXRANKED")
        || mode == QStringLiteral("NORMAL") || mode == QStringLiteral("ARENA")
        || mode == QStringLiteral("ONEFORALL") || mode == QStringLiteral("ULTBOOK");
}

bool decodeChunkedBody(const std::string &raw, QByteArray &body, QString &error)
{
    const size_t separator = raw.find("\r\n\r\n");
    if (separator == std::string::npos) {
        error = QStringLiteral("OP.GG 响应缺少 HTTP 头。" );
        return false;
    }
    const std::string headers = raw.substr(0, separator);
    const std::string bodyText = raw.substr(separator + 4);
    std::string lowerHeaders = headers;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (lowerHeaders.find("transfer-encoding:") == std::string::npos
        || lowerHeaders.find("chunked") == std::string::npos) {
        body = QByteArray(bodyText.data(), static_cast<int>(bodyText.size()));
        return true;
    }

    std::string decoded;
    size_t offset = 0;
    while (offset < bodyText.size()) {
        const size_t lineEnd = bodyText.find("\r\n", offset);
        if (lineEnd == std::string::npos) {
            error = QStringLiteral("OP.GG 分块响应不完整。" );
            return false;
        }
        std::string sizeText = bodyText.substr(offset, lineEnd - offset);
        const size_t extension = sizeText.find(';');
        if (extension != std::string::npos) sizeText.resize(extension);
        size_t first = 0;
        while (first < sizeText.size() && std::isspace(static_cast<unsigned char>(sizeText[first]))) ++first;
        size_t last = sizeText.size();
        while (last > first && std::isspace(static_cast<unsigned char>(sizeText[last - 1]))) --last;
        if (first == last) {
            error = QStringLiteral("OP.GG 分块长度无效。" );
            return false;
        }
        size_t chunkSize = 0;
        for (size_t index = first; index < last; ++index) {
            const char character = sizeText[index];
            unsigned int digit = 0;
            if (character >= '0' && character <= '9') digit = static_cast<unsigned int>(character - '0');
            else if (character >= 'a' && character <= 'f') digit = static_cast<unsigned int>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') digit = static_cast<unsigned int>(character - 'A' + 10);
            else {
                error = QStringLiteral("OP.GG 分块长度无效。" );
                return false;
            }
            if (chunkSize > (64U * 1024U * 1024U - digit) / 16U) {
                error = QStringLiteral("OP.GG 响应过大。" );
                return false;
            }
            chunkSize = chunkSize * 16U + digit;
        }
        offset = lineEnd + 2;
        if (chunkSize == 0) {
            body = QByteArray(decoded.data(), static_cast<int>(decoded.size()));
            return true;
        }
        if (chunkSize > bodyText.size() - offset || bodyText.size() - offset - chunkSize < 2
            || bodyText.compare(offset + chunkSize, 2, "\r\n") != 0) {
            error = QStringLiteral("OP.GG 分块响应不完整。" );
            return false;
        }
        decoded.append(bodyText, offset, chunkSize);
        offset += chunkSize + 2;
    }
    error = QStringLiteral("OP.GG 分块响应缺少结束块。" );
    return false;
}

int numericValue(const QJsonValue &value)
{
    bool ok = false;
    if (value.isString()) {
        QString text = value.toString().trimmed();
        text.remove(',');
        if (text.endsWith('%')) text.chop(1);
        const int result = text.toInt(&ok);
        return ok ? result : 0;
    }
    const int result = value.toVariant().toInt(&ok);
    return ok ? result : 0;
}

double doubleValue(const QJsonValue &value)
{
    bool ok = false;
    double result = value.toVariant().toDouble(&ok);
    if (!ok && value.isString()) {
        QString text = value.toString().trimmed();
        const bool hasPercent = text.endsWith('%');
        if (hasPercent) text.chop(1);
        text.remove(',');
        result = text.toDouble(&ok);
    }
    return ok && std::isfinite(result) ? result : 0.0;
}

QString textValue(const QJsonValue &value)
{
    return value.isString() ? value.toString().trimmed() : QString{};
}

QString imageUrlValue(const QJsonValue &value, const int depth = 0)
{
    if (depth > 4) return {};
    if (value.isString()) return value.toString().trimmed();
    if (!value.isObject()) return {};
    const QJsonObject object = value.toObject();
    for (const char *key : {"src", "url", "href", "image_url", "imageUrl", "iconUrl", "icon_url"}) {
        const QString url = imageUrlValue(object.value(QLatin1String(key)), depth + 1);
        if (!url.isEmpty()) return url;
    }
    return {};
}

QString lanePositionFromValue(const QJsonValue &value, const int depth = 0)
{
    if (depth > 5) return {};
    if (value.isString()) return normalizedPosition(value.toString());
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            const QString position = lanePositionFromValue(entry, depth + 1);
            if (!position.isEmpty()) return position;
        }
        return {};
    }
    if (!value.isObject()) return {};

    const QJsonObject object = value.toObject();
    // Position descriptors are sometimes objects ({name: "SUPPORT"}) rather
    // than strings. Inspect only lane-shaped keys here; walking every nested
    // value would turn a champion class such as MAGE into a false lane.
    for (const char *key : {"position", "positionName", "position_name", "lane", "laneName",
                            "lane_name", "selectedPosition", "selected_position", "selectedLane",
                            "selected_lane", "defaultSelectedPosition", "default_position", "rolePosition",
                            "role_position", "name", "code", "key", "value"}) {
        const QString position = lanePositionFromValue(object.value(QLatin1String(key)), depth + 1);
        if (!position.isEmpty()) return position;
    }
    return {};
}

QString lanePositionFromObject(const QJsonObject &object)
{
    // `role` is also used for OP.GG class labels (MAGE, CONTROLLER, ...), so
    // it is only accepted when it is unambiguously one of the five lanes.
    for (const char *key : {"position", "positionName", "position_name", "lane", "laneName", "lane_name",
                            "selectedPosition", "selected_position", "selectedLane", "selected_lane",
                            "defaultSelectedPosition", "default_position", "rolePosition", "role_position"}) {
        const QString normalized = lanePositionFromValue(object.value(QLatin1String(key)));
        if (!normalized.isEmpty()) return normalized;
    }
    // A few compact API revisions expose the lane under `role`; only accept
    // it through the same strict normalizer so class labels remain untouched.
    return lanePositionFromValue(object.value(QStringLiteral("role")));
}

QString normalizedTier(const QJsonValue &value)
{
    QString text;
    if (value.isString()) {
        text = value.toString().trimmed();
    } else if (value.isDouble()) {
        text = QString::number(value.toInt());
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const char *key : {"tier", "name", "label", "value"}) {
            const QJsonValue nested = object.value(QLatin1String(key));
            if (!nested.isUndefined() && !nested.isNull()) {
                text = normalizedTier(nested);
                if (!text.isEmpty()) break;
            }
        }
    }
    if (text.isEmpty()) return {};

    const QString compact = text.toUpper().trimmed().remove(' ').remove('_').remove('-');
    if (compact == QStringLiteral("0") || compact == QStringLiteral("OP") || compact == QStringLiteral("OPGOD")
        || compact == QStringLiteral("OPGG")) return QStringLiteral("OP");
    if (compact == QStringLiteral("TIER0")) return QStringLiteral("OP");
    if (compact == QStringLiteral("1") || compact == QStringLiteral("TIER1")) return QStringLiteral("T1");
    if (compact == QStringLiteral("2") || compact == QStringLiteral("TIER2")) return QStringLiteral("T2");
    if (compact.startsWith(QStringLiteral("TIER"))) {
        const QString number = compact.mid(4);
        if (number.toInt() > 0) return QStringLiteral("T") + number;
    }
    if (compact.toInt() > 0) return QStringLiteral("T") + compact;
    return text;
}

QJsonValue firstValue(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull()) return value;
    }
    return {};
}

int nestedId(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const char *key : {"id", "key", "value", "perkId", "runeId", "itemId"}) {
            const int id = numericValue(object.value(QLatin1String(key)));
            if (id > 0) return id;
        }
    }
    return numericValue(value);
}

double percentage(const QJsonValue &value)
{
    const double raw = doubleValue(value);
    if (raw <= 0.0) return 0.0;
    return raw <= 1.0 ? raw * 100.0 : raw;
}

void appendUnique(QList<int> &target, int value)
{
    if (value > 0 && !target.contains(value)) target.append(value);
}

int lcuSummonerSpellId(int opggId, const QString &name = {})
{
    const QString normalized = name.trimmed().toLower();
    if (normalized.contains("flash") || normalized.contains(QString::fromUtf8("闪现"))) return 4;
    if (normalized.contains("exhaust") || normalized.contains(QString::fromUtf8("虚弱"))) return 3;
    if (normalized.contains("heal") || normalized.contains(QString::fromUtf8("治疗"))) return 7;
    if (normalized.contains("ignite") || normalized.contains(QString::fromUtf8("点燃"))
        || normalized.contains(QString::fromUtf8("引燃"))) return 14;
    if (normalized.contains("barrier") || normalized.contains(QString::fromUtf8("屏障"))) return 21;
    if (normalized.contains("cleanse") || normalized.contains(QString::fromUtf8("净化"))) return 1;
    if (normalized.contains("ghost") || normalized.contains(QString::fromUtf8("幽灵"))) return 6;
    if (normalized.contains("teleport") || normalized.contains(QString::fromUtf8("传送"))) return 12;
    if (normalized.contains("smite") || normalized.contains(QString::fromUtf8("惩戒"))) return 11;
    if (normalized.contains("clarity") || normalized.contains(QString::fromUtf8("清晰术"))) return 13;
    if (normalized.contains("mark") || normalized.contains("snowball") || normalized.contains(QString::fromUtf8("雪球"))) return 32;
    // Current OP.GG web metadata uses these ids for Exhaust and Flash when
    // the name is not present in a compact API response.
    if (opggId == 73) return 3;
    if (opggId == 74) return 4;
    return opggId;
}

void normalizeSummonerSpellIds(QList<int> &ids)
{
    for (int &id : ids) id = lcuSummonerSpellId(id);
    QList<int> unique;
    for (const int id : ids) appendUnique(unique, id);
    ids = std::move(unique);
}

QString nestedSpellName(const QJsonValue &value, int depth = 0)
{
    if (depth > 8) return {};
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const char *key : {"name", "displayName", "title", "alt", "spellName"}) {
            const QString text = textValue(object.value(QLatin1String(key)));
            if (!text.isEmpty()) return text;
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString text = nestedSpellName(it.value(), depth + 1);
            if (!text.isEmpty()) return text;
        }
    } else if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            const QString text = nestedSpellName(entry, depth + 1);
            if (!text.isEmpty()) return text;
        }
    }
    return {};
}

void collectNumericIds(const QJsonValue &value, QList<int> &target, int depth = 0)
{
    if (depth > 5) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectNumericIds(entry, target, depth + 1);
        return;
    }
    if (!value.isObject()) {
        appendUnique(target, numericValue(value));
        return;
    }
    const QJsonObject object = value.toObject();
    const int direct = nestedId(object);
    if (direct > 0) appendUnique(target, direct);
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key().contains("id", Qt::CaseInsensitive) || it.key().contains("key", Qt::CaseInsensitive)) {
            appendUnique(target, nestedId(it.value()));
        } else if (it.value().isArray() || it.value().isObject()) {
            collectNumericIds(it.value(), target, depth + 1);
        }
    }
}

void collectObjects(const QJsonValue &value, QList<QJsonObject> &objects, int depth = 0)
{
    if (depth > 8) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectObjects(entry, objects, depth + 1);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        objects.append(object);
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isArray() || it.value().isObject()) collectObjects(it.value(), objects, depth + 1);
        }
    }
}

QJsonDocument embeddedJson(const QByteArray &payload)
{
    QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isNull()) return document;
    const QString html = QString::fromUtf8(payload);
    const QRegularExpression scriptPattern(
        R"(<script[^>]+id=["']__NEXT_DATA__["'][^>]*>(.*?)</script>)",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = scriptPattern.match(html);
    if (match.hasMatch()) return QJsonDocument::fromJson(match.captured(1).toUtf8());
    return {};
}

// Next.js app-router pages stream their data as JavaScript string literals
// instead of putting it in __NEXT_DATA__. Decode those strings and collect
// balanced JSON object fragments from the React-flight records.
void collectJsonObjectsFromText(const QByteArray &text, QList<QJsonObject> &objects)
{
    QList<int> stack;
    bool inString = false;
    bool escaped = false;
    constexpr int maxObjects = 100000;
    for (int index = 0; index < text.size() && objects.size() < maxObjects; ++index) {
        const char character = text.at(index);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{') {
            stack.append(index);
        } else if (character == '}' && !stack.isEmpty()) {
            const int start = stack.takeLast();
            const QByteArray fragment = text.mid(start, index - start + 1);
            const QJsonDocument document = QJsonDocument::fromJson(fragment);
            if (!document.isNull() && document.isObject()) objects.append(document.object());
        }
    }
}

QList<QString> embeddedFlightTexts(const QByteArray &payload)
{
    QList<QString> result;
    const QString html = QString::fromUtf8(payload);
    const QRegularExpression flightPattern(
        R"REGEX(self\.__next_f\.push\(\[1,"((?:\\.|[^"\\])*)"\]\))REGEX",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator = flightPattern.globalMatch(html);
    while (iterator.hasNext()) {
        const QString encodedString = iterator.next().captured(1);
        const QJsonDocument decoded = QJsonDocument::fromJson(
            (QStringLiteral("[\"") + encodedString + QStringLiteral("\"]")).toUtf8());
        if (!decoded.isNull() && decoded.isArray() && !decoded.array().isEmpty()
            && decoded.array().first().isString()) {
            result.append(decoded.array().first().toString());
        }
    }
    return result;
}

QList<QJsonObject> embeddedObjects(const QByteArray &payload)
{
    QList<QJsonObject> objects;
    const QJsonDocument document = embeddedJson(payload);
    if (!document.isNull()) {
        collectObjects(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()), objects);
        // A page can contain a small __NEXT_DATA__ bootstrap object as well
        // as the actual app-router records. Keep scanning HTML for the latter.
        const QByteArray trimmed = payload.trimmed();
        if (!trimmed.startsWith('<')) return objects;
    }

    for (const QString &flightText : embeddedFlightTexts(payload)) {
        collectJsonObjectsFromText(flightText.toUtf8(), objects);
    }
    return objects;
}

QJsonObject mergeChampionObject(const QJsonObject &object)
{
    QJsonObject merged = object;
    for (const char *key : {"champion", "character", "hero"}) {
        const QJsonObject nested = object.value(QLatin1String(key)).toObject();
        for (auto it = nested.constBegin(); it != nested.constEnd(); ++it) {
            if (!merged.contains(it.key())) merged.insert(it.key(), it.value());
        }
    }
    return merged;
}

QString opggRoleCode(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) return {};

    const QString original = value;
    if (original == QStringLiteral("辅助")) return QStringLiteral("CONTROLLER");
    if (original == QStringLiteral("坦克")) return QStringLiteral("TANK");
    if (original == QStringLiteral("AD战士")) return QStringLiteral("FIGHTER");
    if (original == QStringLiteral("AP战士")) return QStringLiteral("FIGHTER");
    if (original == QStringLiteral("AP输出")) return QStringLiteral("MAGE");
    if (original == QStringLiteral("AD刺客")) return QStringLiteral("SLAYER");
    if (original == QStringLiteral("AP刺客")) return QStringLiteral("SLAYER");
    if (original == QStringLiteral("AD输出")) return QStringLiteral("MARKSMAN");
    if (original == QStringLiteral("其他")) return QStringLiteral("OTHER");

    value = value.toUpper();
    value.replace('-', ' ');
    value.replace('_', ' ');
    value = value.simplified();
    if (value == QStringLiteral("CONTROLLER") || value == QStringLiteral("SUPPORT")) return QStringLiteral("CONTROLLER");
    if (value == QStringLiteral("TANK") || value == QStringLiteral("VANGUARD")
        || value == QStringLiteral("WARDEN")) return QStringLiteral("TANK");
    if (value == QStringLiteral("FIGHTER") || value == QStringLiteral("DIVER")) return QStringLiteral("FIGHTER");
    if (value == QStringLiteral("MAGE")) return QStringLiteral("MAGE");
    if (value == QStringLiteral("SLAYER") || value == QStringLiteral("ASSASSIN")) return QStringLiteral("SLAYER");
    if (value == QStringLiteral("MARKSMAN") || value == QStringLiteral("ADC")
        || value == QStringLiteral("AD CARRY")) return QStringLiteral("MARKSMAN");
    if (value == QStringLiteral("OTHER") || value == QStringLiteral("SPECIALIST")) return QStringLiteral("OTHER");
    return {};
}

QString opggRoleLabel(QString value)
{
    const QString original = value.trimmed();

    // The Chinese OP.GG page exposes these exact labels in its Class filter.
    // Accepting the labels as-is also lets cached/localized payloads pass
    // through without translating them twice.
    static const QSet<QString> localizedLabels = {
        QStringLiteral("辅助"), QStringLiteral("坦克"), QStringLiteral("AD战士"),
        QStringLiteral("AP战士"),
        QStringLiteral("AP输出"), QStringLiteral("AD刺客"), QStringLiteral("AD输出"),
        QStringLiteral("AP刺客"),
        QStringLiteral("其他")
    };
    if (localizedLabels.contains(original)) return original;

    // A few localized/API revisions use the plain Chinese class names
    // instead of the labels from the Class selector. Normalize those aliases
    // before falling back to the English role code table.
    if (original == QStringLiteral("法师")) return QStringLiteral("AP输出");
    if (original == QStringLiteral("战士")) return QStringLiteral("AD战士");
    if (original == QStringLiteral("刺客")) return QStringLiteral("AD刺客");
    if (original == QStringLiteral("射手") || original == QStringLiteral("远程")) return QStringLiteral("AD输出");

    const QString code = opggRoleCode(original);
    if (code == QStringLiteral("CONTROLLER")) return QStringLiteral("辅助");
    if (code == QStringLiteral("TANK")) return QStringLiteral("坦克");
    if (code == QStringLiteral("FIGHTER")) return QStringLiteral("AD战士");
    if (code == QStringLiteral("MAGE")) return QStringLiteral("AP输出");
    if (code == QStringLiteral("SLAYER")) return QStringLiteral("AD刺客");
    if (code == QStringLiteral("MARKSMAN")) return QStringLiteral("AD输出");
    if (code == QStringLiteral("OTHER")) return QStringLiteral("其他");
    return {};
}

void appendUniqueRole(QStringList &target, const QString &role)
{
    if (!role.isEmpty() && !target.contains(role)) target.append(role);
}

QStringList opggRolesFromValue(const QJsonValue &value, const int depth = 0)
{
    if (depth > 6) return {};
    QStringList result;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            for (const QString &role : opggRolesFromValue(entry, depth + 1)) {
                appendUniqueRole(result, role);
            }
        }
        return result;
    }
    if (value.isString()) {
        const QStringList parts = value.toString().split(QRegularExpression(QStringLiteral("[,/|;]")), Qt::SkipEmptyParts);
        for (const QString &part : parts) appendUniqueRole(result, opggRoleLabel(part));
        return result;
    }
    if (!value.isObject()) return result;

    const QJsonObject object = value.toObject();
    for (const char *key : {"code", "key", "name", "label", "value", "role", "class", "type"}) {
        const QJsonValue nested = object.value(QLatin1String(key));
        if (nested.isString()) appendUniqueRole(result, opggRoleLabel(nested.toString()));
    }
    // Some payload revisions wrap the role code one level deeper.
    if (result.isEmpty()) {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isArray() || it.value().isObject()) {
                for (const QString &role : opggRolesFromValue(it.value(), depth + 1)) {
                    appendUniqueRole(result, role);
                }
            }
        }
    }
    return result;
}

QStringList opggRolesFromObject(const QJsonObject &raw)
{
    const QJsonObject object = mergeChampionObject(raw);
    QStringList result;
    for (const char *key : {"roles", "roleTypes", "role_types", "roleList", "role_list",
                            "championRoles", "champion_roles", "classes", "classTypes", "class_types",
                            "championClass", "champion_class", "championType", "champion_type",
                            "tags", "role", "class", "className", "class_name"}) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull()) continue;
        for (const QString &role : opggRolesFromValue(value)) appendUniqueRole(result, role);
    }
    return result;
}

QString championNameFrom(const QJsonObject &object)
{
    const QJsonObject merged = mergeChampionObject(object);
    return textValue(firstValue(merged, {"championName", "champion_name", "displayName", "name", "championKey", "championSlug", "slug", "key"}));
}

QString championKeyFrom(const QJsonObject &object)
{
    const QJsonObject merged = mergeChampionObject(object);
    return textValue(firstValue(merged, {"championKey", "championSlug", "champion_slug", "slug", "key", "alias"}));
}

bool plausibleChampionId(const int id)
{
    // Live champion ids are below 1000.  Values in the 60000 range are
    // arena variants, while values such as rune ids (8229) are unrelated
    // page metadata and must never become a selectable champion.
    return id > 0 && id < 2000;
}

int championIdFrom(const QJsonObject &object)
{
    const QJsonObject merged = mergeChampionObject(object);
    for (const char *key : {"championId", "championID", "champion_id"}) {
        const int id = numericValue(merged.value(QLatin1String(key)));
        if (plausibleChampionId(id)) return id;
    }
    const QJsonValue championKey = merged.value(QStringLiteral("championKey"));
    const int keyId = numericValue(championKey);
    if (plausibleChampionId(keyId)) return keyId;
    return 0;
}

bool hasChampionIdentity(const OpggChampionRank &rank)
{
    return rank.championId > 0 || !rank.championName.trimmed().isEmpty() || !rank.championKey.trimmed().isEmpty();
}

bool hasChampionIdentity(const OpggChampionReference &champion)
{
    return champion.championId > 0 || !champion.championName.trimmed().isEmpty() || !champion.championKey.trimmed().isEmpty();
}

void appendCounter(QList<OpggChampionReference> &target, OpggChampionReference reference)
{
    if (!hasChampionIdentity(reference)) return;
    const auto duplicate = std::find_if(target.cbegin(), target.cend(), [&reference](const OpggChampionReference &current) {
        if (reference.championId > 0 && current.championId > 0) return reference.championId == current.championId;
        const QString key = !reference.championKey.isEmpty() ? reference.championKey : reference.championName;
        const QString currentKey = !current.championKey.isEmpty() ? current.championKey : current.championName;
        return !key.isEmpty() && key.compare(currentKey, Qt::CaseInsensitive) == 0;
    });
    if (duplicate == target.cend()) target.append(std::move(reference));
}

void collectCounters(const QJsonValue &value, QList<OpggChampionReference> &target, int depth = 0)
{
    if (depth > 4 || target.size() >= 8) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectCounters(entry, target, depth + 1);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    OpggChampionReference reference;
    reference.championId = championIdFrom(object);
    reference.championName = championNameFrom(object);
    reference.championKey = championKeyFrom(object);
    reference.winRate = percentage(firstValue(object, {"winRate", "win_rate", "winrate"}));
    reference.championWinRate = percentage(firstValue(object, {
        "championWinRate", "champion_win_rate", "selfWinRate", "myWinRate"
    }));
    reference.games = numericValue(firstValue(object, {"games", "gameCount", "matches", "play"}));
    const int wins = numericValue(firstValue(object, {"wins", "win", "victories"}));
    const bool hasPlayWinCounts = object.contains(QStringLiteral("play"))
        && object.contains(QStringLiteral("win")) && reference.games > 0;
    if (hasPlayWinCounts) {
        // Ranking rows expose the matchup as {play, win}, where win is the
        // selected champion's win count.  Keep the two sides explicit so the
        // UI can identify a genuine disadvantage instead of comparing the
        // same percentage with itself.
        reference.championWinRate = 100.0 * static_cast<double>(wins) / reference.games;
        reference.winRate = 100.0 - reference.championWinRate;
        reference.relation = reference.championWinRate < 50.0
            ? QStringLiteral("counter") : QStringLiteral("favorable");
    }
    if (reference.championWinRate <= 0.0 && wins > 0 && reference.games > 0) {
        reference.championWinRate = 100.0 * static_cast<double>(wins) / reference.games;
    }
    if (reference.winRate <= 0.0 && reference.championWinRate > 0.0) {
        reference.winRate = reference.championWinRate;
    }
    reference.relation = textValue(firstValue(object, {"relation", "matchup", "type", "side"}));
    if (reference.relation.isEmpty() && object.contains(QStringLiteral("play"))) {
        reference.relation = QStringLiteral("counter");
    }
    appendCounter(target, std::move(reference));
    for (const char *key : {"champion", "opponent", "counter", "target", "data"}) {
        const QJsonValue child = object.value(QLatin1String(key));
        if (child.isArray() || child.isObject()) collectCounters(child, target, depth + 1);
    }
}

bool isStatShardId(const int id)
{
    // Riot reserves the 5000 range for the three stat-shard rows.  Keeping
    // this in one place also handles OP.GG revisions that call the field
    // `selectedPerkIds` without exposing a separate shard array.
    return id >= 5000 && id < 5100;
}

void inferStatShards(OpggBuild &build)
{
    for (const int id : build.runeIds) {
        if (isStatShardId(id)) appendUnique(build.statShardIds, id);
    }
}

QString decodeHtmlEntities(QString value)
{
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    value.replace(QStringLiteral("&#x27;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    value.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return value.trimmed();
}

QHash<QString, QString> localizedRoleLabelsFromMarkup(const QByteArray &payload)
{
    QHash<QString, QString> result;
    const QString html = QString::fromUtf8(payload);
    if (!html.contains(QStringLiteral("<option"), Qt::CaseInsensitive)) return result;

    const QRegularExpression optionPattern(
        QStringLiteral("<option\\b[^>]*>(.*?)</option\\s*>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression valuePattern(
        QStringLiteral("\\bvalue\\s*=\\s*([\\\"'])(.*?)\\1"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator options = optionPattern.globalMatch(html);
    while (options.hasNext()) {
        const QRegularExpressionMatch option = options.next();
        const QRegularExpressionMatch value = valuePattern.match(option.captured(0));
        if (!value.hasMatch()) continue;
        const QString code = opggRoleCode(value.captured(2));
        if (code.isEmpty()) continue;

        QString label = option.captured(1);
        label.remove(QRegularExpression(QStringLiteral("<!--[\\s\\S]*?-->")));
        label.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
        label = decodeHtmlEntities(label).simplified();
        if (label.isEmpty() || label == QStringLiteral("所有定位") || label == QStringLiteral("全部定位")) continue;
        result.insert(code, label);
    }
    return result;
}

void applyLocalizedRoleLabels(QStringList &roles, const QHash<QString, QString> &labels)
{
    if (labels.isEmpty() || roles.isEmpty()) return;
    QStringList localized;
    for (const QString &role : roles) {
        // Keep an explicit localized label intact. AP战士/AP刺客 share an
        // internal class code with their AD counterparts, but the page label
        // is still the more precise value for the user-facing detail view.
        if (role.trimmed() == QStringLiteral("AP战士")
            || role.trimmed() == QStringLiteral("AP刺客")) {
            appendUniqueRole(localized, role.trimmed());
            continue;
        }
        const QString code = opggRoleCode(role);
        const QString label = labels.value(code, role);
        appendUniqueRole(localized, label);
    }
    roles = std::move(localized);
}

struct CounterGroups {
    QList<OpggChampionReference> weak;
    QList<OpggChampionReference> favorable;
};

CounterGroups parseCounterMarkupGroups(const QByteArray &payload)
{
    CounterGroups groups;
    const QString html = QString::fromUtf8(payload);
    const QRegularExpression sectionPattern(
        QStringLiteral("<section\\b[^>]*>(.*?)</section>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression linkPattern(
        QStringLiteral("<a\\b[^>]*href=[\\\"'][^\\\"']*target_champion=([^&\\\"']+)[^\\\"']*[\\\"'][^>]*>(.*?)</a\\s*>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression altPattern(
        QStringLiteral("<img\\b[^>]*\\balt=[\\\"']([^\\\"']+)[\\\"']"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression percentPattern(
        QStringLiteral("<(?:strong|span)\\b[^>]*>\\s*(\\d+(?:\\.\\d+)?)%\\s*</(?:strong|span)>"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression gamesPattern(
        QStringLiteral("<span\\b[^>]*>\\s*([\\d,]+)\\s*</span>\\s*<span\\b[^>]*>\\s*(?:场|Games)\\s*</span>"),
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatchIterator sections = sectionPattern.globalMatch(html);
    while (sections.hasNext()) {
        const QString section = sections.next().captured(1);
        const bool isDisadvantage = section.contains(QStringLiteral("劣势对抗"), Qt::CaseInsensitive)
            || section.contains(QStringLiteral("weak"), Qt::CaseInsensitive);
        if (!section.contains(QStringLiteral("target_champion="), Qt::CaseInsensitive)) continue;

        QRegularExpressionMatchIterator links = linkPattern.globalMatch(section);
        while (links.hasNext()) {
            const QRegularExpressionMatch link = links.next();
            const QString fragment = link.captured(2);
            const QRegularExpressionMatch alt = altPattern.match(fragment);
            const QRegularExpressionMatch percent = percentPattern.match(fragment);
            const QRegularExpressionMatch games = gamesPattern.match(fragment);
            OpggChampionReference reference;
            reference.championKey = decodeHtmlEntities(link.captured(1));
            reference.championName = alt.hasMatch() ? decodeHtmlEntities(alt.captured(1)) : reference.championKey;
            if (percent.hasMatch()) {
                reference.championWinRate = percent.captured(1).toDouble();
                reference.winRate = 100.0 - reference.championWinRate;
                reference.relation = isDisadvantage
                    ? QStringLiteral("counter") : QStringLiteral("favorable");
            }
            if (games.hasMatch()) reference.games = games.captured(1).remove(',').toInt();
            if (isDisadvantage) {
                if (groups.weak.size() < 5) appendCounter(groups.weak, std::move(reference));
            } else if (groups.favorable.size() < 5) {
                appendCounter(groups.favorable, std::move(reference));
            }
        }
    }
    return groups;
}

QList<OpggChampionReference> parseCounterMarkup(const QByteArray &payload)
{
    const CounterGroups groups = parseCounterMarkupGroups(payload);
    return groups.weak.isEmpty() ? groups.favorable : groups.weak;
}

OpggChampionRank rankFromObject(const QJsonObject &raw, const QString &position)
{
    const QJsonObject object = mergeChampionObject(raw);
    OpggChampionRank result;
    result.championId = championIdFrom(object);
    result.championName = championNameFrom(object);
    result.championKey = championKeyFrom(object);
    result.roles = opggRolesFromObject(object);
    result.position = lanePositionFromObject(object);
    if (result.position.isEmpty()) result.position = position;
    result.rank = numericValue(firstValue(object, {"rank", "ranking", "order", "tierRank", "positionRank"}));
    QJsonObject tierData = object.value("positionTierData").toObject();
    if (tierData.isEmpty()) tierData = object.value("tierData").toObject();
    if (tierData.isEmpty()) {
        const QJsonArray tierArray = object.value("positionTierData").toArray();
        if (!tierArray.isEmpty()) tierData = tierArray.first().toObject();
    }
    result.tier = normalizedTier(firstValue(object, {"tier", "grade", "tierName", "positionTier"}));
    if (result.tier.isEmpty()) result.tier = normalizedTier(firstValue(tierData, {"tier", "tierName", "grade"}));
    if (result.rank <= 0) result.rank = numericValue(firstValue(tierData, {"rank", "positionRank"}));
    // OP.GG's current app-router payload stores the position-specific rates
    // already as percentages (for example 0.758 means 0.758%), while older
    // compact API payloads use ratios (0.12 means 12%).  Do not run the
    // position fields through the ratio converter or pick/ban rates become
    // 100 times too large.
    const auto positionRate = [&object](const char *positionKey,
                                         std::initializer_list<const char *> fallbackKeys) {
        const QJsonValue value = object.value(QLatin1String(positionKey));
        if (!value.isUndefined() && !value.isNull()) return doubleValue(value);
        return percentage(firstValue(object, fallbackKeys));
    };
    result.winRate = positionRate("positionWinRate", {"winRate", "win_rate", "winrate"});
    result.pickRate = positionRate("positionPickRate", {"pickRate", "pick_rate", "pickrate"});
    result.banRate = positionRate("positionBanRate", {"banRate", "ban_rate", "banrate"});
    result.games = numericValue(firstValue(object, {"games", "gameCount", "play", "matches"}));
    const QJsonObject stats = object.value("stats").toObject();
    if (result.winRate <= 0.0) result.winRate = percentage(firstValue(stats, {"winRate", "win_rate", "winrate", "positionWinRate"}));
    if (result.pickRate <= 0.0) result.pickRate = percentage(firstValue(stats, {"pickRate", "pick_rate", "pickrate", "positionPickRate"}));
    if (result.banRate <= 0.0) result.banRate = percentage(firstValue(stats, {"banRate", "ban_rate", "banrate", "positionBanRate"}));
    if (result.games <= 0) result.games = numericValue(firstValue(stats, {"games", "gameCount", "matches"}));
    for (const char *key : {"positionCounters", "counters", "counterChampions", "counter_champions",
                            "weakAgainst"}) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isArray() || value.isObject()) collectCounters(value, result.weakAgainst);
    }
    const QJsonValue strongValue = object.value(QStringLiteral("strongAgainst"));
    if (strongValue.isArray() || strongValue.isObject()) collectCounters(strongValue, result.strongAgainst);
    std::stable_sort(result.weakAgainst.begin(), result.weakAgainst.end(),
                     [](const OpggChampionReference &left, const OpggChampionReference &right) {
        if (left.championWinRate > 0.0 && right.championWinRate > 0.0
            && !qFuzzyCompare(left.championWinRate + 1.0, right.championWinRate + 1.0)) {
            return left.championWinRate < right.championWinRate;
        }
        return left.games > right.games;
    });
    return result;
}

bool isRankRecordObject(const QJsonObject &object)
{
    const QJsonObject merged = mergeChampionObject(object);
    // App-router ranking rows have these position-specific fields.  Nested
    // positionCounters/items/runes objects do not, and must never become
    // leaderboard rows.
    for (const char *key : {"positionName", "positionWinRate", "positionPickRate", "positionBanRate",
                            "positionTierData", "positionTier", "positionRank", "positionRoleRate"}) {
        if (merged.contains(QLatin1String(key))) return true;
    }
    // Keep compatibility with the compact JSON API and fixture payloads.
    const bool hasStats = merged.contains(QStringLiteral("winRate"))
        || merged.contains(QStringLiteral("win_rate"))
        || merged.contains(QStringLiteral("pickRate"))
        || merged.contains(QStringLiteral("pick_rate"));
    const bool hasRank = merged.contains(QStringLiteral("rank"))
        || merged.contains(QStringLiteral("ranking"))
        || merged.contains(QStringLiteral("tierRank"));
    const bool looksLikeCounter = merged.contains(QStringLiteral("play"))
        && merged.contains(QStringLiteral("win"))
        && !hasRank;
    return hasStats && (hasRank || !looksLikeCounter)
        && !merged.contains(QStringLiteral("metaType"));
}

bool likelyRankObject(const OpggChampionRank &rank)
{
    if (!hasChampionIdentity(rank)
        || (rank.rank <= 0 && rank.winRate <= 0.0 && rank.pickRate <= 0.0 && rank.games <= 0)) {
        return false;
    }

    // The embedded page payload contains rune/item/stat objects alongside the
    // champion rows. They often have a generic `name` and a numeric `rank` or
    // `play` field, so accepting every named object would turn values such as
    // "Rune" and "英雄 8229" into selectable champions.
    const QString name = rank.championName.trimmed().toCaseFolded();
    const QString key = rank.championKey.trimmed().toCaseFolded();
    static const QRegularExpression metadataName(
        QStringLiteral("^(?:rune|item|spell|perk|shard|fragment|英雄\\s*\\d+|符文|装备|技能|属性碎片)"),
        QRegularExpression::CaseInsensitiveOption);
    if (metadataName.match(name).hasMatch() || metadataName.match(key).hasMatch()) return false;
    if (rank.championId <= 0 && rank.championKey.trimmed().isEmpty()) return !name.isEmpty();
    return true;
}

QList<OpggChampionRank> rankingForPosition(const QList<OpggChampionRank> &ranking, const QString &position)
{
    QList<OpggChampionRank> filtered;
    bool hasPositionData = false;
    for (const OpggChampionRank &entry : ranking) {
        const QString entryPosition = normalizedPosition(entry.position);
        if (!entryPosition.isEmpty()) hasPositionData = true;
        if (position.isEmpty() || entryPosition.isEmpty() || entryPosition == position) filtered.append(entry);
    }
    // Some API revisions omit positionName even when the request is scoped.
    // In that case retaining the response is more useful than showing an empty table.
    if (!position.isEmpty() && (!hasPositionData || filtered.isEmpty())) return ranking;

    // Collapse role duplicates for the mode-wide view (and any API response
    // that repeats the same champion without a stable numeric id).
    QList<OpggChampionRank> unique;
    for (const OpggChampionRank &entry : filtered) {
        const auto duplicate = std::find_if(unique.cbegin(), unique.cend(), [&entry](const OpggChampionRank &existing) {
            if (entry.championId > 0 && existing.championId > 0) return entry.championId == existing.championId;
            return !entry.championName.isEmpty()
                && entry.championName.compare(existing.championName, Qt::CaseInsensitive) == 0;
        });
        if (duplicate == unique.cend()) unique.append(entry);
    }
    return unique.isEmpty() && !filtered.isEmpty() ? filtered : unique;
}

QList<int> idsForKeys(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    QList<int> result;
    for (const char *key : keys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isUndefined()) collectNumericIds(value, result);
    }
    return result;
}

void collectSpellIds(const QJsonValue &value, QList<int> &result, int depth = 0)
{
    if (depth > 8) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectSpellIds(entry, result, depth + 1);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    const QString type = textValue(object.value("metaType"));
    if (type.compare(QStringLiteral("spell"), Qt::CaseInsensitive) == 0) {
        const int id = nestedId(object.value("metaId"));
        appendUnique(result, lcuSummonerSpellId(id, nestedSpellName(object)));
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) collectSpellIds(it.value(), result, depth + 1);
    }
}

void appendRawIds(const QJsonValue &value, QList<int> &result, const int depth = 0)
{
    if (depth > 8) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) appendRawIds(entry, result, depth + 1);
        return;
    }
    if (value.isObject()) {
        const int id = nestedId(value);
        if (id > 0) {
            result.append(id);
            return;
        }
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isArray() || it.value().isObject()) appendRawIds(it.value(), result, depth + 1);
        }
        return;
    }
    const int id = numericValue(value);
    if (id > 0) result.append(id);
}

bool jsonBool(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toInt() != 0;
    return value.toString().trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value.toString().trimmed() == QStringLiteral("1");
}

void collectActiveIds(const QJsonValue &value, QList<int> &result, const int depth = 0)
{
    if (depth > 20) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectActiveIds(entry, result, depth + 1);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    if (jsonBool(object.value(QStringLiteral("isActive")))
        || jsonBool(object.value(QStringLiteral("active")))) {
        const int id = nestedId(value);
        if (id > 0) result.append(id);
        return;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) collectActiveIds(it.value(), result, depth + 1);
    }
}

QList<QList<int>> runeRowsFromValue(const QJsonValue &value)
{
    QList<QList<int>> rows;
    if (!value.isArray()) return rows;

    // Current OP.GG payloads expose each rune tree as an array of rows, with
    // each row containing objects such as {id, isActive}.  Keep the row
    // boundaries: flattening the tree is what previously made multiple
    // runes appear selected in one row and also made old hard-coded ids win.
    for (const QJsonValue &rowValue : value.toArray()) {
        if (!rowValue.isArray()) continue;
        QList<int> row;
        for (const QJsonValue &runeValue : rowValue.toArray()) {
            const int id = nestedId(runeValue);
            if (id > 0 && !row.contains(id)) row.append(id);
        }
        if (!row.isEmpty()) rows.append(std::move(row));
    }
    return rows;
}

QList<QList<OpggRuneChoice>> runeChoiceRowsFromValue(const QJsonValue &value)
{
    QList<QList<OpggRuneChoice>> rows;
    if (!value.isArray()) return rows;
    for (const QJsonValue &rowValue : value.toArray()) {
        if (!rowValue.isArray()) continue;
        QList<OpggRuneChoice> row;
        for (const QJsonValue &runeValue : rowValue.toArray()) {
            if (!runeValue.isObject()) continue;
            const QJsonObject object = runeValue.toObject();
            OpggRuneChoice choice;
            choice.id = nestedId(runeValue);
            choice.name = textValue(firstValue(object, {"name", "displayName", "label", "title", "alt"}));
            choice.imageUrl = imageUrlValue(firstValue(object, {
                "image_url", "imageUrl", "image", "icon", "iconUrl", "icon_url"}));
            choice.active = jsonBool(firstValue(object, {"isActive", "active", "selected"}));
            choice.pickRate = percentage(firstValue(object, {"pick_rate", "pickRate", "pickrate"}));
            choice.winRate = percentage(firstValue(object, {"win_rate", "winRate", "winrate"}));
            choice.games = numericValue(firstValue(object, {"play", "games", "gameCount", "matches"}));
            if (choice.isValid()) row.append(std::move(choice));
        }
        if (!row.isEmpty()) rows.append(std::move(row));
    }
    return rows;
}

QList<int> activeIdsFromRuneRows(const QList<QList<OpggRuneChoice>> &rows)
{
    QList<int> result;
    for (const QList<OpggRuneChoice> &row : rows) {
        for (const OpggRuneChoice &choice : row) {
            if (choice.active && choice.id > 0) {
                result.append(choice.id);
                break;
            }
        }
    }
    return result;
}

void keepFirstSelectionPerRow(QList<int> &selected, const QList<QList<int>> &rows)
{
    for (const QList<int> &row : rows) {
        bool found = false;
        for (const int id : row) {
            if (!selected.contains(id)) continue;
            if (!found) {
                found = true;
            } else {
                selected.removeAll(id);
            }
        }
    }
}

QJsonObject buildSourceObject(const QJsonObject &raw)
{
    QJsonObject source = mergeChampionObject(raw);
    const QJsonObject imported = raw.value(QStringLiteral("importClientData")).toObject();
    for (auto it = imported.constBegin(); it != imported.constEnd(); ++it) {
        if (!source.contains(it.key())) source.insert(it.key(), it.value());
    }
    return source;
}

OpggRuneBuild runeBuildFromObject(const QJsonObject &raw)
{
    const QJsonObject source = buildSourceObject(raw);
    const QJsonObject primaryStyle = source.value(QStringLiteral("primary_perk_style")).toObject();
    const QJsonObject subStyle = source.value(QStringLiteral("perk_sub_style")).toObject();
    OpggRuneBuild result;
    result.primaryStyleId = nestedId(firstValue(source, {
        "primaryStyleId", "primaryStyle", "primaryRuneStyle", "primary", "primary_perk_style"}));
    if (result.primaryStyleId <= 0) result.primaryStyleId = nestedId(primaryStyle.value(QStringLiteral("id")));
    result.subStyleId = nestedId(firstValue(source, {
        "subStyleId", "subStyle", "secondaryStyle", "secondary", "perk_sub_style"}));
    if (result.subStyleId <= 0) result.subStyleId = nestedId(subStyle.value(QStringLiteral("id")));

    result.primaryChoices = runeChoiceRowsFromValue(source.value(QStringLiteral("main_runes")));
    result.subChoices = runeChoiceRowsFromValue(source.value(QStringLiteral("sub_runes")));
    result.statChoices = runeChoiceRowsFromValue(source.value(QStringLiteral("shards")));
    for (const QList<OpggRuneChoice> &row : result.primaryChoices) {
        QList<int> ids;
        for (const OpggRuneChoice &choice : row) if (choice.id > 0) ids.append(choice.id);
        if (!ids.isEmpty()) result.primaryRows.append(std::move(ids));
    }
    for (const QList<OpggRuneChoice> &row : result.subChoices) {
        QList<int> ids;
        for (const OpggRuneChoice &choice : row) if (choice.id > 0) ids.append(choice.id);
        if (!ids.isEmpty()) result.subRows.append(std::move(ids));
    }
    for (const QList<OpggRuneChoice> &row : result.statChoices) {
        QList<int> ids;
        for (const OpggRuneChoice &choice : row) if (choice.id > 0) ids.append(choice.id);
        if (!ids.isEmpty()) result.statRows.append(std::move(ids));
    }

    // The active flags are the authoritative selection on current OP.GG
    // pages.  They retain duplicate shard ids in their original slot order.
    result.runeIds = activeIdsFromRuneRows(result.primaryChoices);
    const QList<int> activeSub = activeIdsFromRuneRows(result.subChoices);
    for (const int id : activeSub) result.runeIds.append(id);
    if (result.runeIds.isEmpty()) {
        const QJsonValue selected = firstValue(source, {"selectedPerkIds", "selectedRuneIds", "runeIds", "perkIds"});
        if (!selected.isUndefined()) appendRawIds(selected, result.runeIds);
        if (result.runeIds.isEmpty()) {
            collectActiveIds(source.value(QStringLiteral("main_runes")), result.runeIds);
            collectActiveIds(source.value(QStringLiteral("sub_runes")), result.runeIds);
        }
    }
    const QJsonValue explicitShards = firstValue(source, {"statShardIds", "statShards", "fragmentIds"});
    result.statShardIds.clear();
    for (const QList<OpggRuneChoice> &row : result.statChoices) {
        for (const OpggRuneChoice &choice : row) {
            if (choice.active && isStatShardId(choice.id)) {
                result.statShardIds.append(choice.id);
                break;
            }
        }
    }
    if (result.statShardIds.isEmpty() && !explicitShards.isUndefined() && !explicitShards.isNull()) {
        appendRawIds(explicitShards, result.statShardIds);
    } else if (result.statShardIds.isEmpty()) {
        const QJsonValue shards = source.value(QStringLiteral("shards"));
        collectActiveIds(shards, result.statShardIds);
        if (result.statShardIds.isEmpty()) appendRawIds(shards, result.statShardIds);
    }
    QList<int> shardIdsFromRunes;
    for (const int id : result.runeIds) if (isStatShardId(id)) appendUnique(shardIdsFromRunes, id);
    // Only use shard ids from selectedPerkIds as a fallback.  Do not use
    // appendUnique here: two adaptive shards in different slots are valid.
    for (const int id : shardIdsFromRunes) {
        if (!result.statShardIds.contains(id)) result.statShardIds.append(id);
    }
    result.runeIds.erase(std::remove_if(result.runeIds.begin(), result.runeIds.end(), [](const int id) {
        return isStatShardId(id);
    }), result.runeIds.end());
    if (result.primaryStyleId > 0) result.runeIds.removeAll(result.primaryStyleId);
    if (result.subStyleId > 0) result.runeIds.removeAll(result.subStyleId);
    keepFirstSelectionPerRow(result.runeIds, result.primaryRows);
    keepFirstSelectionPerRow(result.runeIds, result.subRows);

    collectSpellIds(raw, result.summonerSpellIds);
    normalizeSummonerSpellIds(result.summonerSpellIds);
    result.winRate = percentage(firstValue(raw, {"winRate", "win_rate", "winrate"}));
    result.pickRate = percentage(firstValue(raw, {"pickRate", "pick_rate", "pickrate"}));
    result.games = numericValue(firstValue(raw, {"games", "gameCount", "play", "matches"}));
    result.label = textValue(firstValue(raw, {"label", "name", "title"}));
    return result;
}

void appendRuneBuildUnique(QList<OpggRuneBuild> &target, OpggRuneBuild candidate)
{
    if (!candidate.isValid()) return;
    const auto duplicate = std::find_if(target.begin(), target.end(), [&candidate](OpggRuneBuild &existing) {
        return candidate.primaryStyleId == existing.primaryStyleId
            && candidate.subStyleId == existing.subStyleId
            && candidate.runeIds == existing.runeIds
            && candidate.statShardIds == existing.statShardIds;
    });
    if (duplicate == target.end()) {
        target.append(std::move(candidate));
        return;
    }
    if (duplicate->summonerSpellIds.isEmpty() && !candidate.summonerSpellIds.isEmpty()) {
        duplicate->summonerSpellIds = candidate.summonerSpellIds;
    }
    if (duplicate->primaryRows.isEmpty() && !candidate.primaryRows.isEmpty()) {
        duplicate->primaryRows = candidate.primaryRows;
    }
    if (duplicate->subRows.isEmpty() && !candidate.subRows.isEmpty()) {
        duplicate->subRows = candidate.subRows;
    }
    if (duplicate->statRows.isEmpty() && !candidate.statRows.isEmpty()) {
        duplicate->statRows = candidate.statRows;
    }
    if (duplicate->primaryChoices.isEmpty() && !candidate.primaryChoices.isEmpty()) {
        duplicate->primaryChoices = candidate.primaryChoices;
    }
    if (duplicate->subChoices.isEmpty() && !candidate.subChoices.isEmpty()) {
        duplicate->subChoices = candidate.subChoices;
    }
    if (duplicate->statChoices.isEmpty() && !candidate.statChoices.isEmpty()) {
        duplicate->statChoices = candidate.statChoices;
    }
    if (duplicate->pickRate <= 0.0) duplicate->pickRate = candidate.pickRate;
    if (duplicate->winRate <= 0.0) duplicate->winRate = candidate.winRate;
    if (duplicate->games <= 0) duplicate->games = candidate.games;
}

void collectRuneBuildsFromValue(const QJsonValue &value, QList<OpggRuneBuild> &target,
                                const double inheritedPickRate = 0.0,
                                const int inheritedGames = 0, const int depth = 0)
{
    if (depth > 10) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            collectRuneBuildsFromValue(entry, target, inheritedPickRate, inheritedGames, depth + 1);
        }
        return;
    }
    if (!value.isObject()) return;

    const QJsonObject object = value.toObject();
    const double ownPickRate = percentage(firstValue(object, {"pickRate", "pick_rate", "pickrate"}));
    const int ownGames = numericValue(firstValue(object, {"games", "gameCount", "play", "matches"}));
    const double pickRate = ownPickRate > 0.0 ? ownPickRate : inheritedPickRate;
    const int games = ownGames > 0 ? ownGames : inheritedGames;
    OpggRuneBuild candidate = runeBuildFromObject(object);
    if (candidate.isValid()) {
        if (candidate.pickRate <= 0.0) candidate.pickRate = pickRate;
        if (candidate.games <= 0) candidate.games = games;
        appendRuneBuildUnique(target, std::move(candidate));
    }

    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) {
            collectRuneBuildsFromValue(it.value(), target, pickRate, games, depth + 1);
        }
    }
}

int balancedObjectEnd(const QString &text, const int start)
{
    if (start < 0 || start >= text.size() || text.at(start) != QLatin1Char('{')) return -1;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int index = start; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (inString) {
            if (escaped) escaped = false;
            else if (character == QLatin1Char('\\')) escaped = true;
            else if (character == QLatin1Char('"')) inString = false;
            continue;
        }
        if (character == QLatin1Char('"')) inString = true;
        else if (character == QLatin1Char('{')) ++depth;
        else if (character == QLatin1Char('}') && --depth == 0) return index;
    }
    return -1;
}

void collectItemMetaIds(const QJsonValue &value, QList<int> &target, const int depth = 0)
{
    if (depth > 32) return;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectItemMetaIds(entry, target, depth + 1);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    const QString metaType = textValue(object.value(QStringLiteral("metaType")));
    const int metaId = numericValue(object.value(QStringLiteral("metaId")));
    if (metaId > 0 && (metaType.isEmpty() || metaType.compare(QStringLiteral("item"), Qt::CaseInsensitive) == 0)) {
        appendUnique(target, metaId);
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) collectItemMetaIds(it.value(), target, depth + 1);
    }
}

struct ItemFlightRow {
    QString key;
    int group{};
    int depth{};
    int index{};
    QList<int> ids;
    QList<OpggItemBuild::Choice> choices;
    double pickRate{};
    double winRate{};
    int games{};
};

QList<OpggItemBuild::Choice> itemChoicesFromRowText(const QString &rowText)
{
    QList<OpggItemBuild::Choice> choices;
    // Current React-flight rows put metaType/metaId next to the image alt
    // text.  Keep a second expression for the older id-before-type order.
    const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\\"metaType\\\"\\s*:\\s*\\\"item\\\"\\s*,\\s*\\\"metaId\\\"\\s*:\\s*(\\d+)([\\s\\S]{0,700}?)\\\"alt\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""),
                            QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("\\\"metaId\\\"\\s*:\\s*(\\d+)([\\s\\S]{0,700}?)\\\"metaType\\\"\\s*:\\s*\\\"item\\\"([\\s\\S]{0,700}?)\\\"alt\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""),
                            QRegularExpression::CaseInsensitiveOption)
    };
    QRegularExpressionMatchIterator iterator = patterns.constFirst().globalMatch(rowText);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        OpggItemBuild::Choice choice;
        choice.itemId = match.captured(1).toInt();
        choice.name = decodeHtmlEntities(match.captured(3));
        if (choice.itemId > 0) choices.append(std::move(choice));
    }
    if (choices.isEmpty()) {
        iterator = patterns.at(1).globalMatch(rowText);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            OpggItemBuild::Choice choice;
            choice.itemId = match.captured(1).toInt();
            choice.name = decodeHtmlEntities(match.captured(4));
            if (choice.itemId > 0) choices.append(std::move(choice));
        }
    }

    // A few cached rows omit the alt attribute.  Recover ids in their
    // original order and leave the name empty so the repository can provide a
    // localized fallback in the view.
    if (choices.isEmpty()) {
        const QRegularExpression idPattern(
            QStringLiteral("\\\"metaType\\\"\\s*:\\s*\\\"item\\\"\\s*,\\s*\\\"metaId\\\"\\s*:\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        iterator = idPattern.globalMatch(rowText);
        while (iterator.hasNext()) {
            OpggItemBuild::Choice choice;
            choice.itemId = iterator.next().captured(1).toInt();
            if (choice.itemId > 0) choices.append(std::move(choice));
        }
    }
    return choices;
}

void itemRowStatsFromText(const QString &rowText, double &pickRate, double &winRate, int &games)
{
    const QRegularExpression percentPattern(QStringLiteral("(\\d+(?:\\.\\d+)?)%"));
    QRegularExpressionMatchIterator percentages = percentPattern.globalMatch(rowText);
    QList<double> values;
    while (percentages.hasNext() && values.size() < 4) values.append(percentages.next().captured(1).toDouble());
    if (!values.isEmpty()) pickRate = values.at(0);
    if (values.size() > 1) winRate = values.at(1);
    const QRegularExpression gamesPattern(QStringLiteral("([\\d,]+)[^\\d]{0,40}(?:场|Games)"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch gamesMatch = gamesPattern.match(rowText);
    if (gamesMatch.hasMatch()) games = gamesMatch.captured(1).remove(',').toInt();
}

QList<ItemFlightRow> itemRowsFromFlightTexts(const QList<QString> &texts)
{
    QList<ItemFlightRow> rows;
    const QRegularExpression keyPattern(
        R"REGEX("((?:starter_items|boots|support_items|core_items|depth_[0-9]+_item)_[0-9]+)")REGEX");
    const QRegularExpression partsPattern(
        R"REGEX(^(starter_items|boots|support_items|core_items|depth_([0-9]+)_item)_([0-9]+)$)REGEX");
    for (const QString &text : texts) {
        QRegularExpressionMatchIterator iterator = keyPattern.globalMatch(text);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch keyMatch = iterator.next();
            const QString key = keyMatch.captured(1);
            const QRegularExpressionMatch parts = partsPattern.match(key);
            if (!parts.hasMatch()) continue;
            const int objectStart = text.indexOf(QLatin1Char('{'), keyMatch.capturedEnd());
            const int objectEnd = balancedObjectEnd(text, objectStart);
            if (objectStart < 0 || objectEnd < 0) continue;
            const QJsonDocument document = QJsonDocument::fromJson(
                text.mid(objectStart, objectEnd - objectStart + 1).toUtf8());
            if (!document.isObject()) continue;
            QList<int> ids;
            collectItemMetaIds(document.object(), ids);
            if (ids.isEmpty()) continue;
            if (std::find_if(rows.cbegin(), rows.cend(), [&key](const ItemFlightRow &row) {
                    return row.key == key;
                }) != rows.cend()) continue;
            ItemFlightRow row;
            row.key = key;
            row.index = parts.captured(3).toInt();
            if (parts.captured(1) == QStringLiteral("starter_items")) row.group = 0;
            else if (parts.captured(1) == QStringLiteral("boots")) row.group = 1;
            else if (parts.captured(1) == QStringLiteral("support_items")) row.group = 2;
            else if (parts.captured(1) == QStringLiteral("core_items")) row.group = 3;
            else {
                row.group = 4;
                row.depth = parts.captured(2).toInt();
            }
            row.ids = std::move(ids);
            row.choices = itemChoicesFromRowText(text.mid(objectStart, objectEnd - objectStart + 1));
            if (row.choices.isEmpty()) {
                for (const int id : row.ids) {
                    OpggItemBuild::Choice choice;
                    choice.itemId = id;
                    row.choices.append(std::move(choice));
                }
            }
            itemRowStatsFromText(text.mid(objectStart, objectEnd - objectStart + 1),
                                 row.pickRate, row.winRate, row.games);
            for (OpggItemBuild::Choice &choice : row.choices) {
                choice.pickRate = row.pickRate;
                choice.winRate = row.winRate;
                choice.games = row.games;
            }
            rows.append(std::move(row));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ItemFlightRow &left, const ItemFlightRow &right) {
        if (left.group != right.group) return left.group < right.group;
        if (left.depth != right.depth) return left.depth < right.depth;
        return left.index < right.index;
    });
    return rows;
}

void appendItemSequence(QList<int> &target, const QList<int> &source)
{
    for (const int id : source) if (id > 0) target.append(id);
}

QList<OpggItemBuild> itemBuildsFromFlightTexts(const QList<QString> &texts)
{
    const QList<ItemFlightRow> rows = itemRowsFromFlightTexts(texts);
    if (rows.isEmpty()) return {};

    OpggItemBuild build;
    build.label = QStringLiteral("OP.GG 推荐出装");
    const auto appendGroup = [&build](const ItemFlightRow &row, const QString &label) {
        OpggItemBuild::Group group;
        group.label = label;
        group.choices = row.choices;
        group.pickRate = row.pickRate;
        group.winRate = row.winRate;
        group.games = row.games;
        if (row.group == 0) build.starterGroups.append(group);
        else if (row.group == 1) build.bootsGroups.append(group);
        else if (row.group == 2) build.supportGroups.append(group);
        else if (row.group == 3) build.coreGroups.append(group);
        else if (row.depth == 4) build.fourthGroups.append(group);
        else if (row.depth == 5) build.fifthGroups.append(group);
        else if (row.depth == 6) build.sixthGroups.append(group);
    };
    const auto idsFromChoices = [](const QList<OpggItemBuild::Choice> &choices) {
        QList<int> ids;
        for (const auto &choice : choices) if (choice.itemId > 0) ids.append(choice.itemId);
        return ids;
    };
    for (const ItemFlightRow &row : rows) {
        const QString label = row.group == 0 ? QStringLiteral("出门装")
            : row.group == 1 ? QStringLiteral("鞋子")
            : row.group == 2 ? QStringLiteral("辅助装")
            : row.group == 3 ? QStringLiteral("核心装备")
            : QStringLiteral("第 %1 件装备").arg(row.depth);
        appendGroup(row, label);
        const QList<int> ids = idsFromChoices(row.choices);
        if (row.group == 0) appendItemSequence(build.starterItemIds, ids);
        else if (row.group == 1) appendItemSequence(build.bootsItemIds, ids);
        else if (row.group == 2) appendItemSequence(build.supportItemIds, ids);
        else if (row.group == 3) appendItemSequence(build.coreItemIds, ids);
        else appendItemSequence(build.finalItemIds, ids);
        appendItemSequence(build.itemIds, ids);
        if (build.pickRate <= 0.0 && row.pickRate > 0.0) build.pickRate = row.pickRate;
        if (build.winRate <= 0.0 && row.winRate > 0.0) build.winRate = row.winRate;
        if (build.games <= 0 && row.games > 0) build.games = row.games;
    }
    return build.isValid() ? QList<OpggItemBuild>{std::move(build)} : QList<OpggItemBuild>{};
}

OpggBuild buildFromObject(const QJsonObject &raw)
{
    const QJsonObject object = buildSourceObject(raw);
    OpggBuild build;
    build.championId = championIdFrom(object);
    build.championName = championNameFrom(object);
    build.championKey = championKeyFrom(object);
    build.roles = opggRolesFromObject(raw);
    build.position = lanePositionFromObject(object);
    build.primaryStyleId = nestedId(firstValue(object, {"primaryStyleId", "primaryStyle", "primaryRuneStyle", "primary", "primary_perk_style"}));
    build.subStyleId = nestedId(firstValue(object, {"subStyleId", "subStyle", "secondaryStyle", "secondary", "perk_sub_style"}));
    build.runeIds = idsForKeys(object, {"selectedPerkIds", "selectedRuneIds", "runeIds", "perkIds", "perks", "runes"});
    if (build.runeIds.isEmpty()) {
        collectActiveIds(object.value(QStringLiteral("main_runes")), build.runeIds);
        collectActiveIds(object.value(QStringLiteral("sub_runes")), build.runeIds);
    }
    build.statShardIds = idsForKeys(object, {"statShardIds", "statShards", "shards", "fragmentIds"});
    build.summonerSpellIds = idsForKeys(object, {"summonerSpellIds", "summonerSpells", "summoner_spells", "spells"});
    if (build.summonerSpellIds.isEmpty()) collectSpellIds(raw, build.summonerSpellIds);
    normalizeSummonerSpellIds(build.summonerSpellIds);
    build.itemIds = idsForKeys(object, {"itemIds", "items", "buildItems", "recommendedItems"});
    for (const char *key : {"counters", "counterChampions", "counter_champions", "matchups", "weakAgainst", "strongAgainst"}) {
        const QJsonValue counters = object.value(QLatin1String(key));
        if (counters.isArray() || counters.isObject()) collectCounters(counters, build.counters);
    }
    for (const OpggChampionReference &counter : build.counters) {
        const QString relation = counter.relation.toLower();
        if (relation.contains(QStringLiteral("favor")) || relation.contains(QStringLiteral("strong"))
            || relation.contains(QStringLiteral("优势"))) build.favorableCounters.append(counter);
        else build.weakCounters.append(counter);
    }
    if (build.primaryStyleId > 0) build.runeIds.removeAll(build.primaryStyleId);
    if (build.subStyleId > 0) build.runeIds.removeAll(build.subStyleId);
    inferStatShards(build);

    // OP.GG's current page keeps the selected perk ids in importClientData
    // and the active flags in main_runes/sub_runes. Prefer that structured
    // representation when it is available so duplicated shard slots remain
    // intact for one-click application.
    const OpggRuneBuild structuredRune = runeBuildFromObject(raw);
    if (structuredRune.isValid()) {
        build.primaryStyleId = structuredRune.primaryStyleId;
        build.subStyleId = structuredRune.subStyleId;
        // Keep the legacy selectedPerkIds shape (which includes the three
        // shard slots) for one-click import compatibility. The structured
        // variant itself still exposes statShardIds separately for the UI.
        const QJsonValue selectedPerks = firstValue(object, {"selectedPerkIds", "selectedRuneIds"});
        build.runeIds = selectedPerks.isUndefined() || selectedPerks.isNull()
            ? structuredRune.runeIds : idsForKeys(object, {"selectedPerkIds", "selectedRuneIds"});
        if (build.runeIds.isEmpty()) build.runeIds = structuredRune.runeIds;
        build.statShardIds = structuredRune.statShardIds;
        if (!structuredRune.summonerSpellIds.isEmpty()) {
            build.summonerSpellIds = structuredRune.summonerSpellIds;
        }
    }
    return build;
}

QStringList pageRolesFromObjects(const QList<QJsonObject> &objects, const OpggBuild &build,
                                 const QHash<QString, QString> &localizedLabels)
{
    QStringList bestRoles;
    int bestScore = -1;
    const QString wantedKey = build.championKey.trimmed().toCaseFolded();
    const QString wantedName = build.championName.trimmed().toCaseFolded();
    const bool identityKnown = build.championId > 0 || !wantedKey.isEmpty() || !wantedName.isEmpty();
    for (const QJsonObject &object : objects) {
        QStringList roles = opggRolesFromObject(object);
        if (roles.isEmpty()) continue;

        const int objectId = championIdFrom(object);
        const QString objectKey = championKeyFrom(object).trimmed().toCaseFolded();
        const QString objectName = championNameFrom(object).trimmed().toCaseFolded();
        const bool hasIdentity = objectId > 0 || !objectKey.isEmpty() || !objectName.isEmpty();
        const bool sameIdentity = (build.championId > 0 && objectId == build.championId)
            || (!wantedKey.isEmpty() && objectKey == wantedKey)
            || (!wantedName.isEmpty() && objectName == wantedName);
        // A role record explicitly belonging to another champion must never
        // leak into the current detail page.  Identity-free header metadata is
        // allowed because OP.GG stores the selected champion in its parent.
        if (identityKnown && hasIdentity && !sameIdentity) continue;

        int score = sameIdentity ? 100 : (identityKnown ? 0 : 60);
        if (object.contains(QStringLiteral("position"))) score += 20;
        if (object.contains(QStringLiteral("selectedValue"))) score += 2;
        if (object.contains(QStringLiteral("showControls"))) score += 1;
        applyLocalizedRoleLabels(roles, localizedLabels);
        if (score > bestScore) {
            bestScore = score;
            bestRoles = std::move(roles);
        }
    }
    return bestRoles;
}

QString versionFromText(const QString &text)
{
    static const QRegularExpression jsonVersion(
        QStringLiteral("(?:\\\"|\\b)version(?:\\\"|\\b)\\s*:\\s*(?:\\\"|')([0-9]{1,2}\\.[0-9]{1,2}(?:\\.[0-9]+)?)(?:\\\"|')"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch jsonMatch = jsonVersion.match(text);
    if (jsonMatch.hasMatch()) return jsonMatch.captured(1);

    static const QRegularExpression titleVersion(
        QStringLiteral("(?:版本|patch(?:\\s*version)?)\\s*([0-9]{1,2}\\.[0-9]{1,2}(?:\\.[0-9]+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch titleMatch = titleVersion.match(text);
    if (titleMatch.hasMatch()) return titleMatch.captured(1);

    static const QRegularExpression imageVersion(
        QStringLiteral("/meta/images/lol/([0-9]{1,2}\\.[0-9]{1,2})(?:\\.[0-9]+)?/"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch imageMatch = imageVersion.match(text);
    return imageMatch.hasMatch() ? imageMatch.captured(1) : QString{};
}

QString versionFromPayload(const QByteArray &payload)
{
    const QString html = QString::fromUtf8(payload);
    QString version = versionFromText(html);
    if (!version.isEmpty()) return version;
    for (const QString &flightText : embeddedFlightTexts(payload)) {
        version = versionFromText(flightText);
        if (!version.isEmpty()) return version;
    }
    return {};
}

QString normalizedLaneCode(QString value)
{
    value = value.trimmed().toLower();
    if (value == QStringLiteral("bottom")) value = QStringLiteral("adc");
    if (value == QStringLiteral("utility")) value = QStringLiteral("support");
    return value;
}

QList<OpggLaneStat> laneStatsFromMarkup(const QByteArray &payload)
{
    QList<OpggLaneStat> result;
    const QString html = QString::fromUtf8(payload);
    const QRegularExpression routePattern(
        QStringLiteral("href\\s*=\\s*[\\\"'][^\\\"']*/build/(top|jungle|mid|adc|support)[^\\\"']*[\\\"'][^>]*>(?:(?!</a>)[\\s\\S])*?(\\d+(?:\\.\\d+)?)%\\s*</a>"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator = routePattern.globalMatch(html);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        const QString position = normalizedLaneCode(match.captured(1));
        const double pickRate = match.captured(2).toDouble();
        if (position.isEmpty() || pickRate <= 0.0) continue;
        auto existing = std::find_if(result.begin(), result.end(), [&position](const OpggLaneStat &stat) {
            return stat.position == position;
        });
        if (existing == result.end()) result.append({position, pickRate, 0});
        else existing->pickRate = qMax(existing->pickRate, pickRate);
    }
    std::sort(result.begin(), result.end(), [](const OpggLaneStat &left, const OpggLaneStat &right) {
        return left.pickRate > right.pickRate;
    });
    return result;
}

struct PagePositionMetadata {
    QString selectedPosition;
    QList<OpggLaneStat> laneStats;
};

void appendLaneStat(QList<OpggLaneStat> &target, QString position, const double pickRate,
                    const int games = 0)
{
    position = normalizedPosition(position);
    if (position.isEmpty() || pickRate <= 0.0) return;
    auto existing = std::find_if(target.begin(), target.end(), [&position](const OpggLaneStat &stat) {
        return stat.position == position;
    });
    if (existing == target.end()) {
        target.append({position, pickRate, games});
    } else {
        existing->pickRate = qMax(existing->pickRate, pickRate);
        existing->games = qMax(existing->games, games);
    }
}

PagePositionMetadata positionMetadataFromObjects(const QList<QJsonObject> &objects,
                                                 const OpggBuild &build)
{
    PagePositionMetadata best;
    int bestScore = -1;
    const QString wantedKey = build.championKey.trimmed().toCaseFolded();
    const QString wantedName = build.championName.trimmed().toCaseFolded();
    const QString wantedPosition = normalizedPosition(build.position);
    const bool identityKnown = build.championId > 0 || !wantedKey.isEmpty() || !wantedName.isEmpty();
    for (const QJsonObject &object : objects) {
        const QJsonValue positionsValue = object.value(QStringLiteral("positions"));
        if (!positionsValue.isArray()) continue;

        // App-router pages contain position metadata for the selected
        // champion as well as generic navigation/related-champion objects.
        // An identity-bearing object that is not the selected champion must
        // never win simply because it appears later in the flight stream.
        const int objectId = championIdFrom(object);
        const QString objectKey = championKeyFrom(object).trimmed().toCaseFolded();
        const QString objectName = championNameFrom(object).trimmed().toCaseFolded();
        const bool hasIdentity = objectId > 0 || !objectKey.isEmpty() || !objectName.isEmpty();
        const bool sameIdentity = (build.championId > 0 && objectId == build.championId)
            || (!wantedKey.isEmpty() && objectKey == wantedKey)
            || (!wantedName.isEmpty() && objectName == wantedName);
        // If the build card did not carry an identity, the page-level
        // position object is the only usable source.  Rejecting every
        // identity-bearing object in that situation produced an empty or
        // unrelated lane split.  Once an identity is known, keep the strict
        // match so another champion's positions cannot leak in.
        if (identityKnown && hasIdentity && !sameIdentity) continue;

        QList<OpggLaneStat> stats;
        for (const QJsonValue &entry : positionsValue.toArray()) {
            const QJsonObject positionObject = entry.toObject();
            if (positionObject.isEmpty()) continue;
            const QString rawPosition = textValue(firstValue(positionObject,
                {"name", "position", "code", "key", "lane"}));
            const double rate = percentage(firstValue(positionObject,
                {"percentage", "pickRate", "pick_rate", "rate", "value"}));
            appendLaneStat(stats, rawPosition, rate,
                           numericValue(firstValue(positionObject, {"games", "play", "matches"})));
        }
        if (stats.isEmpty()) continue;

        const QString selected = lanePositionFromObject(object);
        int score = sameIdentity ? 100 : (identityKnown ? 20 : 60);
        if (!selected.isEmpty()) score += 20;
        if (!wantedPosition.isEmpty() && normalizedPosition(selected) == wantedPosition) score += 8;
        // Prefer the page-level list over a one-entry widget when both are
        // identity-free. This is what preserves Lux's SUPPORT 75% + MID 25%
        // split when the API response only contains the requested lane.
        score += qMin(5, stats.size());
        // A page-level filter object normally carries a version or selected
        // value alongside `positions`; these fields help prefer it over a
        // generic list embedded in a navigation component.
        if (object.contains(QStringLiteral("version"))) score += 2;
        if (object.contains(QStringLiteral("selectedValue"))) score += 1;
        if (score <= bestScore) continue;
        bestScore = score;
        best.selectedPosition = selected;
        best.laneStats = std::move(stats);
    }
    std::sort(best.laneStats.begin(), best.laneStats.end(), [](const OpggLaneStat &left,
                                                               const OpggLaneStat &right) {
        return left.pickRate > right.pickRate;
    });
    return best;
}

QStringList skillOrderFromMarkup(const QByteArray &payload)
{
    QString html = QString::fromUtf8(payload);
    int start = html.indexOf(QStringLiteral("SkillOrder Table"), 0, Qt::CaseInsensitive);
    if (start < 0) {
        for (const QString &flightText : embeddedFlightTexts(payload)) {
            const int flightStart = flightText.indexOf(QStringLiteral("SkillOrder Table"), 0, Qt::CaseInsensitive);
            if (flightStart >= 0) {
                html = flightText;
                start = flightStart;
                break;
            }
        }
    }
    if (start < 0) return {};
    const int end = html.indexOf(QStringLiteral("</section>"), start, Qt::CaseInsensitive);
    const QString section = html.mid(start, end > start ? end - start : 30000);
    const QRegularExpression skillPattern(
        QStringLiteral("<strong\\b[^>]*>\\s*([QWER])\\s*</strong>"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList sequence;
    QRegularExpressionMatchIterator iterator = skillPattern.globalMatch(section);
    while (iterator.hasNext()) sequence.append(iterator.next().captured(1).toUpper());
    // The first three badges are the max-order preview (E -> Q -> W); the
    // following fifteen badges are the actual level-by-level recommendation.
    if (sequence.size() > 15) sequence = sequence.mid(sequence.size() - 15);
    return sequence;
}

QList<OpggSpellBuild> spellBuildsFromFlightTexts(const QList<QString> &texts)
{
    QList<OpggSpellBuild> result;
    for (const QString &text : texts) {
        if (!text.contains(QStringLiteral("spells_table_"), Qt::CaseInsensitive)) continue;
        const int tableStart = text.indexOf(QStringLiteral("spells_table_"), 0, Qt::CaseInsensitive);
        const QString table = text.mid(tableStart);
        struct SpellToken { int id{}; QString name; QString imageUrl; int start{}; int end{}; };
        QList<SpellToken> tokens;
        const QRegularExpression tokenKey(QStringLiteral("\\\"spell_\\d+\\\""),
                                           QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression idPattern(QStringLiteral("\\\"metaId\\\"\\s*:\\s*(\\d+)"),
                                             QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression altPattern(QStringLiteral("\\\"alt\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""),
                                              QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression srcPattern(QStringLiteral("\\\"src\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""),
                                              QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator iterator = tokenKey.globalMatch(table);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch keyMatch = iterator.next();
            const int start = keyMatch.capturedStart();
            const int nextStart = table.indexOf(QStringLiteral("\"spell_"), keyMatch.capturedEnd());
            const QString segment = table.mid(start, nextStart >= 0 ? nextStart - start : 1800);
            const QRegularExpressionMatch idMatch = idPattern.match(segment);
            if (!idMatch.hasMatch()) continue;
            SpellToken token;
            const QRegularExpressionMatch altMatch = altPattern.match(segment);
            const QRegularExpressionMatch srcMatch = srcPattern.match(segment);
            token.id = lcuSummonerSpellId(idMatch.captured(1).toInt(), altMatch.captured(1));
            token.name = decodeHtmlEntities(altMatch.captured(1));
            token.imageUrl = srcMatch.captured(1);
            token.start = start;
            token.end = nextStart >= 0 ? nextStart : table.size();
            if (token.id > 0) tokens.append(std::move(token));
        }
        for (int index = 0; index + 1 < tokens.size(); index += 2) {
            const SpellToken &first = tokens.at(index);
            const SpellToken &second = tokens.at(index + 1);
            OpggSpellBuild build;
            build.spellIds = {first.id, second.id};
            build.spellNames = {first.name, second.name};
            build.spellImageUrls = {first.imageUrl, second.imageUrl};
            const int segmentEnd = index + 2 < tokens.size() ? tokens.at(index + 2).start : table.size();
            const QString segment = table.mid(second.start, qMax(0, segmentEnd - second.start));
            const QRegularExpression percentPattern(QStringLiteral("(\\d+(?:\\.\\d+)?)%"));
            QRegularExpressionMatchIterator percentages = percentPattern.globalMatch(segment);
            QList<double> rates;
            while (percentages.hasNext() && rates.size() < 3) rates.append(percentages.next().captured(1).toDouble());
            if (!rates.isEmpty()) build.pickRate = rates.at(0);
            if (rates.size() > 1) build.winRate = rates.at(1);
            const QRegularExpression gamesPattern(QStringLiteral("([\\d,]+)[^\\d]{0,40}(?:场|Games)"),
                                                   QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch gamesMatch = gamesPattern.match(segment);
            if (gamesMatch.hasMatch()) build.games = gamesMatch.captured(1).remove(',').toInt();
            if (std::find_if(result.cbegin(), result.cend(), [&build](const OpggSpellBuild &existing) {
                    return existing.spellIds == build.spellIds;
                }) == result.cend()) result.append(std::move(build));
        }
    }
    return result;
}

} // namespace

class OpggCrawler::Impl {
public:
    using RawReply = std::function<void(QByteArray payload, QString error)>;

    Impl() : work_(asio::make_work_guard(context_)), ssl_(asio::ssl::context::tls_client), worker_([this] { context_.run(); })
    {
        // The bundled OpenSSL runtime on Windows does not necessarily have a
        // system CA bundle. This client only reads public OP.GG pages and
        // never sends LCU credentials, so allow the connection to proceed in
        // that environment instead of making the HTML fallback unusable.
        ssl_.set_verify_mode(asio::ssl::verify_none);
    }

    ~Impl()
    {
        work_.reset();
        context_.stop();
        if (worker_.joinable()) worker_.join();
    }

    void fetch(const QString &host, const QString &path, RawReply reply)
    {
        asio::post(context_, [this, host, path, reply = std::move(reply)]() mutable {
            std::make_shared<Request>(context_, ssl_, host, path, std::move(reply))->start();
        });
    }

private:
    struct Request : std::enable_shared_from_this<Request> {
        Request(asio::io_context &io, asio::ssl::context &context, QString host, QString path, RawReply reply)
            : stream(io, context), resolver(io), host(std::move(host)), path(std::move(path)), reply(std::move(reply)) {}

        void start()
        {
            if (SSL_set_tlsext_host_name(stream.native_handle(), host.toUtf8().constData()) != 1) {
                return finish({}, "无法设置 OP.GG TLS 主机名。");
            }
            resolver.async_resolve(host.toStdString(), "443", [self = shared_from_this()](const boost::system::error_code &ec,
                                                                                             const tcp::resolver::results_type &endpoints) {
                if (ec) return self->finish({}, "无法解析 OP.GG：" + QString::fromStdString(ec.message()));
                asio::async_connect(self->stream.next_layer(), endpoints,
                                    [self](const boost::system::error_code &connectError, const tcp::endpoint &) {
                    if (connectError) return self->finish({}, "无法连接 OP.GG：" + QString::fromStdString(connectError.message()));
                    self->stream.async_handshake(asio::ssl::stream_base::client,
                                                 [self](const boost::system::error_code &handshakeError) {
                        if (handshakeError) return self->finish({}, "OP.GG TLS 握手失败：" + QString::fromStdString(handshakeError.message()));
                        self->send();
                    });
                });
            });
        }

        void send()
        {
            request = "GET " + path.toUtf8() + " HTTP/1.1\r\nHost: " + host.toUtf8()
                + "\r\nUser-Agent: Janna/1.0 (OP.GG champion assistant)\r\nAccept: application/json,text/html\r\nConnection: close\r\n\r\n";
            asio::async_write(stream, asio::buffer(request.constData(), static_cast<size_t>(request.size())),
                              [self = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                if (ec) return self->finish({}, "OP.GG 请求发送失败：" + QString::fromStdString(ec.message()));
                self->read();
            });
        }

        void read()
        {
            asio::async_read(stream, response, asio::transfer_all(),
                             [self = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                if (ec != asio::error::eof) return self->finish({}, "OP.GG 响应读取失败：" + QString::fromStdString(ec.message()));
                const std::string raw(asio::buffers_begin(self->response.data()), asio::buffers_end(self->response.data()));
                const size_t separator = raw.find("\r\n\r\n");
                if (separator == std::string::npos || raw.rfind("HTTP/1.1 2", 0) != 0) {
                    return self->finish({}, "OP.GG 返回了非成功响应。");
                }
                QByteArray body;
                QString decodeError;
                if (!decodeChunkedBody(raw, body, decodeError)) return self->finish({}, std::move(decodeError));
                self->finish(std::move(body), {});
            });
        }

        void finish(QByteArray payload, QString error)
        {
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                                      [reply = std::move(reply), payload = std::move(payload), error = std::move(error)] {
                reply(std::move(payload), std::move(error));
            }, Qt::QueuedConnection);
        }

        asio::ssl::stream<tcp::socket> stream;
        tcp::resolver resolver;
        QString host;
        QString path;
        RawReply reply;
        QByteArray request;
        asio::streambuf response;
    };

    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::ssl::context ssl_;
    std::thread worker_;
};

OpggCrawler::OpggCrawler() : impl_(std::make_unique<Impl>()) {}
OpggCrawler::~OpggCrawler() = default;

void OpggCrawler::fetchProfile(const QString &region, const QString &gameName, const QString &tagLine, Reply reply)
{
    const QString profile = "/lol/summoners/" + encoded(region) + "/" + encoded(gameName + "-" + tagLine);
    impl_->fetch("op.gg", profile, [reply = std::move(reply)](QByteArray payload, QString error) mutable {
        reply(QString::fromUtf8(payload), std::move(error));
    });
}

void OpggCrawler::fetchImage(const QString &url, BinaryReply reply)
{
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid() || parsed.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || parsed.host().isEmpty() || parsed.path().isEmpty()) {
        return reply({}, QStringLiteral("OP.GG 图片地址无效。"));
    }
    QString path = parsed.path(QUrl::FullyEncoded);
    if (!parsed.query(QUrl::FullyEncoded).isEmpty()) {
        path += QStringLiteral("?") + parsed.query(QUrl::FullyEncoded);
    }
    impl_->fetch(parsed.host(), path, std::move(reply));
}

void OpggCrawler::fetchChampionRanking(const QString &mode, const QString &position, RankingReply reply)
{
    fetchChampionRanking(mode, position, std::move(reply), QStringLiteral("emerald_plus"), {});
}

void OpggCrawler::fetchChampionRanking(const QString &mode, const QString &position, RankingReply reply,
                                       const QString &rankTier, const QString &version)
{
    const QString normalizedModeValue = normalizedMode(mode);
    if (!opggModeSupported(normalizedModeValue)) {
        return reply({}, QStringLiteral("该游戏模式没有可用的 OP.GG 数据。"));
    }
    const QString normalizedPositionValue = normalizedPosition(position);
    const QString normalizedRankTierValue = normalizedRankTier(rankTier);
    const QString normalizedVersionValue = normalizedVersion(version);
    QString query = "region=kr&tier=" + encoded(normalizedRankTierValue) + "&queue_type=" + encoded(normalizedModeValue)
        + "&mode=" + encoded(normalizedModeValue);
    if (!normalizedVersionValue.isEmpty()) query += "&patch=" + encoded(normalizedVersionValue);
    const QString webType = opggWebType(normalizedModeValue);
    if (!webType.isEmpty()) query += "&type=" + encoded(webType);
    if (!normalizedPositionValue.isEmpty()) query += "&position=" + encoded(normalizedPositionValue);
    const QString apiPath = "/api/v1.0/internal/bypass/champions/global?" + query;
    QString htmlPath;
    if (!webType.isEmpty()) {
        htmlPath = "/lol/champions?type=" + encoded(webType) + "&region=kr&tier=" + encoded(normalizedRankTierValue)
            + (normalizedPositionValue.isEmpty() ? QString{} : "&position=" + encoded(normalizedPositionValue))
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    } else if (!opggModeRoute(normalizedModeValue).isEmpty()) {
        htmlPath = opggModeRoute(normalizedModeValue) + "?region=kr&tier=" + encoded(normalizedRankTierValue)
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    } else {
        htmlPath = "/lol/champions?region=kr&tier=" + encoded(normalizedRankTierValue) + "&queue_type=" + encoded(normalizedModeValue)
            + (normalizedPositionValue.isEmpty() ? QString{} : "&position=" + encoded(normalizedPositionValue))
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    }
    const auto fallback = [this, htmlPath, reply, normalizedPositionValue](QString) {
        impl_->fetch("op.gg", localizedOpggPath(htmlPath), [reply, normalizedPositionValue](QByteArray payload, QString error) mutable {
            if (!error.isEmpty()) return reply({}, std::move(error));
            QString parseError;
            QList<OpggChampionRank> ranking = OpggCrawler::parseChampionRanking(payload, &parseError);
            if (ranking.isEmpty()) return reply({}, parseError.isEmpty() ? "OP.GG 未返回英雄排行榜。" : std::move(parseError));
            ranking = rankingForPosition(ranking, normalizedPositionValue);
            for (OpggChampionRank &entry : ranking) if (entry.position.isEmpty()) entry.position = normalizedPositionValue;
            reply(std::move(ranking), {});
        });
    };
    impl_->fetch("lol-web-api.op.gg", apiPath,
                 [reply, fallback, normalizedPositionValue](QByteArray payload, QString error) mutable {
        if (!error.isEmpty()) return fallback(error);
        QString parseError;
        QList<OpggChampionRank> ranking = OpggCrawler::parseChampionRanking(payload, &parseError);
        if (ranking.isEmpty()) return fallback(parseError);
        ranking = rankingForPosition(ranking, normalizedPositionValue);
        for (OpggChampionRank &entry : ranking) if (entry.position.isEmpty()) entry.position = normalizedPositionValue;
        reply(std::move(ranking), {});
    });
}

void OpggCrawler::fetchChampionBuild(const int championId, const QString &mode, const QString &position, BuildReply reply,
                                     const QString &championSlug)
{
    fetchChampionBuild(championId, mode, position, std::move(reply), championSlug,
                      QStringLiteral("emerald_plus"), {});
}

void OpggCrawler::fetchChampionBuild(const int championId, const QString &mode, const QString &position, BuildReply reply,
                                     const QString &championSlug, const QString &rankTier, const QString &version)
{
    if (championId <= 0) return reply({}, "未选择英雄，无法读取推荐配置。");
    const QString normalizedModeValue = normalizedMode(mode);
    if (!opggModeSupported(normalizedModeValue)) {
        return reply({}, QStringLiteral("该游戏模式没有可用的 OP.GG 数据。"));
    }
    const QString normalizedPositionValue = normalizedPosition(position);
    const QString normalizedRankTierValue = normalizedRankTier(rankTier);
    const QString normalizedVersionValue = normalizedVersion(version);
    QString queueQuery = "region=kr&tier=" + encoded(normalizedRankTierValue) + "&queue_type=" + encoded(normalizedModeValue)
        + "&mode=" + encoded(normalizedModeValue);
    if (!normalizedVersionValue.isEmpty()) queueQuery += "&patch=" + encoded(normalizedVersionValue);
    const QString webType = opggWebType(normalizedModeValue);
    const QString apiPath = "/api/v1.0/internal/bypass/champions/" + QString::number(championId)
        + "/positions/" + (normalizedPositionValue.isEmpty() ? QStringLiteral("all") : normalizedPositionValue)
        + "?" + queueQuery;
    const QString slug = championSlug.trimmed().isEmpty() ? QString::number(championId) : championSlug.trimmed();
    QString htmlPath;
    if (!opggModeRoute(normalizedModeValue).isEmpty()) {
        htmlPath = opggModeRoute(normalizedModeValue) + "/" + encoded(slug) + "/build";
        htmlPath += "?region=kr&tier=" + encoded(normalizedRankTierValue)
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    } else if (!webType.isEmpty()) {
        const QString routePosition = normalizedPositionValue.isEmpty() ? QString{} : "/" + normalizedPositionValue;
        htmlPath = "/lol/champions/" + encoded(slug) + "/build" + routePosition
            + "?position=" + (normalizedPositionValue.isEmpty() ? QStringLiteral("all") : encoded(normalizedPositionValue))
            + "&type=" + encoded(webType) + "&region=kr&tier=" + encoded(normalizedRankTierValue)
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    } else {
        htmlPath = "/lol/champions/" + encoded(slug) + "/build?" + queueQuery
            + (normalizedPositionValue.isEmpty() ? QString{} : "&position=" + encoded(normalizedPositionValue));
    }
    QString alternateHtmlPath;
    if (normalizedModeValue == "NORMAL" && !webType.isEmpty()) {
        const QString routePosition = normalizedPositionValue.isEmpty() ? QString{} : "/" + normalizedPositionValue;
        alternateHtmlPath = "/lol/champions/" + encoded(slug) + "/build" + routePosition
            + "?position=" + (normalizedPositionValue.isEmpty() ? QStringLiteral("all") : encoded(normalizedPositionValue))
            + "&type=ranked&region=kr&tier=" + encoded(normalizedRankTierValue)
            + (normalizedVersionValue.isEmpty() ? QString{} : "&patch=" + encoded(normalizedVersionValue));
    }
    QString counterPath;
    if (!slug.isEmpty() && (normalizedModeValue == QStringLiteral("SOLORANKED")
                            || normalizedModeValue == QStringLiteral("FLEXRANKED")
                            || normalizedModeValue == QStringLiteral("NORMAL"))) {
        const QString counterType = normalizedModeValue == QStringLiteral("FLEXRANKED")
            ? QStringLiteral("flex") : normalizedModeValue == QStringLiteral("NORMAL")
                ? QStringLiteral("classic") : QStringLiteral("ranked");
        const QString counterPosition = normalizedPositionValue.isEmpty()
            ? QString{} : QStringLiteral("/") + normalizedPositionValue;
        counterPath = QStringLiteral("/lol/champions/") + encoded(slug) + QStringLiteral("/counters")
            + counterPosition + QStringLiteral("?region=kr&type=") + encoded(counterType)
            + QStringLiteral("&tier=") + encoded(normalizedRankTierValue)
            + (normalizedVersionValue.isEmpty() ? QString{} : QStringLiteral("&patch=") + encoded(normalizedVersionValue));
    }
    auto finishBuild = [this, counterPath, reply, normalizedRankTierValue](OpggBuild build, QString buildError) mutable {
        if (build.rankTier.isEmpty()) build.rankTier = normalizedRankTierValue;
        if (!buildError.isEmpty() || counterPath.isEmpty()) return reply(std::move(build), std::move(buildError));
        impl_->fetch("op.gg", localizedOpggPath(counterPath),
                     [reply, build = std::move(build)](QByteArray payload, QString counterError) mutable {
            if (counterError.isEmpty()) {
                const CounterGroups groups = parseCounterMarkupGroups(payload);
                if (!groups.weak.isEmpty() || !groups.favorable.isEmpty()) {
                    build.weakCounters = groups.weak;
                    build.favorableCounters = groups.favorable;
                    build.counters = groups.weak;
                    for (const OpggChampionReference &reference : groups.favorable) {
                        if (std::none_of(build.counters.cbegin(), build.counters.cend(),
                                         [&reference](const OpggChampionReference &current) {
                            return current.championId > 0 && current.championId == reference.championId;
                        })) build.counters.append(reference);
                    }
                } else {
                    QString counterParseError;
                    const QList<OpggChampionReference> counters = OpggCrawler::parseCounterPage(payload, &counterParseError);
                    if (!counters.isEmpty()) {
                        build.counters = counters;
                        build.weakCounters = counters;
                    }
                }
            }
            // Counter data is supplementary.  A transient counter-page
            // failure must not discard an otherwise valid rune build.
            reply(std::move(build), {});
        });
    };
    // The internal JSON endpoint is fast and contains the selected build, but
    // it often omits the page-level class selector and the other lane shares
    // (for example Lux returns only SUPPORT there).  The localized page is the
    // authoritative source for those two pieces of metadata.  Enrich a valid
    // API build before exposing it to the UI, while retaining all of the API
    // rune/item details if the supplementary request is unavailable.
    auto enrichPageMetadata = [this, htmlPath, finishBuild, championId,
                               normalizedPositionValue, normalizedRankTierValue,
                               normalizedVersionValue](OpggBuild build) mutable {
        if (htmlPath.isEmpty()) return finishBuild(std::move(build), {});
        impl_->fetch("op.gg", localizedOpggPath(htmlPath),
                     [finishBuild, build = std::move(build), championId,
                      normalizedPositionValue, normalizedRankTierValue,
                     normalizedVersionValue](QByteArray payload, QString error) mutable {
            if (error.isEmpty()) {
                QString pageError;
                const OpggBuild page = OpggCrawler::parseBuild(payload, &pageError);
                if (!page.roles.isEmpty()) build.roles = page.roles;
                // The localized champion page is the authoritative source
                // for the complete lane split.  The internal endpoint is
                // commonly scoped to the requested lane and therefore may
                // contain only one entry (or a stale lane value).  Keeping
                // that value when the page has any lane metadata is what
                // caused otherwise correct heroes to display the wrong
                // position/share combination.
                if (!page.laneStats.isEmpty()) build.laneStats = page.laneStats;
                if (!page.position.isEmpty()) build.position = page.position;
                if (!page.version.isEmpty()) build.version = page.version;
                if (build.championId <= 0 && page.championId > 0) build.championId = page.championId;
                if (build.championKey.isEmpty()) build.championKey = page.championKey;
                if (build.championName.isEmpty()) build.championName = page.championName;
                // The API normally has the richer build cards. Fill only
                // missing sections from the page so a transient API shape
                // change cannot erase a valid recommendation.
                if (build.runeBuilds.isEmpty()) build.runeBuilds = page.runeBuilds;
                if (build.itemBuilds.isEmpty()) build.itemBuilds = page.itemBuilds;
                if (build.skillOrder.isEmpty()) build.skillOrder = page.skillOrder;
                if (build.spellBuilds.isEmpty()) build.spellBuilds = page.spellBuilds;
                if (build.summonerSpellIds.size() < 2 && page.summonerSpellIds.size() >= 2) {
                    build.summonerSpellIds = page.summonerSpellIds;
                }
            }
            if (build.championId <= 0) build.championId = championId;
            if (build.position.isEmpty()) build.position = normalizedPositionValue;
            if (build.rankTier.isEmpty()) build.rankTier = normalizedRankTierValue;
            if (build.version.isEmpty()) build.version = normalizedVersionValue;
            finishBuild(std::move(build), {});
        });
    };
    auto fallback = [this, htmlPath, alternateHtmlPath, finishBuild, championId, normalizedPositionValue,
                     normalizedRankTierValue, normalizedVersionValue](QString) mutable {
        impl_->fetch("op.gg", localizedOpggPath(htmlPath),
                     [this, alternateHtmlPath, finishBuild, championId, normalizedPositionValue,
                      normalizedRankTierValue, normalizedVersionValue](QByteArray payload, QString error) mutable {
            QString parseError;
            OpggBuild build = error.isEmpty() ? OpggCrawler::parseBuild(payload, &parseError) : OpggBuild{};
            const bool usable = error.isEmpty() && build.primaryStyleId > 0 && !build.runeIds.isEmpty();
            if ((!error.isEmpty() || !usable) && !alternateHtmlPath.isEmpty()) {
                impl_->fetch("op.gg", localizedOpggPath(alternateHtmlPath),
                             [finishBuild, championId, normalizedPositionValue, normalizedRankTierValue,
                              normalizedVersionValue](QByteArray alternatePayload, QString alternateError) mutable {
                    QString alternateParseError;
                    OpggBuild alternate = alternateError.isEmpty()
                        ? OpggCrawler::parseBuild(alternatePayload, &alternateParseError) : OpggBuild{};
                    if (!alternateError.isEmpty()) return finishBuild({}, std::move(alternateError));
                    if (alternate.primaryStyleId <= 0 || alternate.runeIds.isEmpty()) {
                        return finishBuild({}, alternateParseError.isEmpty()
                            ? QStringLiteral("OP.GG 未返回该英雄的推荐配置。") : std::move(alternateParseError));
                    }
                    if (alternate.championId <= 0) alternate.championId = championId;
                    if (alternate.position.isEmpty()) alternate.position = normalizedPositionValue;
                    alternate.rankTier = normalizedRankTierValue;
                    if (alternate.version.isEmpty()) alternate.version = normalizedVersionValue;
                    finishBuild(std::move(alternate), {});
                });
                return;
            }
            if (!error.isEmpty()) return finishBuild({}, std::move(error));
            if (!build.isValid() || !usable) {
                return finishBuild({}, parseError.isEmpty()
                    ? QStringLiteral("OP.GG 未返回该英雄的推荐配置。") : std::move(parseError));
            }
            if (build.championId <= 0) build.championId = championId;
            if (build.position.isEmpty()) build.position = normalizedPositionValue;
            build.rankTier = normalizedRankTierValue;
            if (build.version.isEmpty()) build.version = normalizedVersionValue;
            finishBuild(std::move(build), {});
        });
    };
    impl_->fetch("lol-web-api.op.gg", apiPath,
                 [fallback, finishBuild, enrichPageMetadata, championId, normalizedModeValue,
                  normalizedPositionValue, normalizedRankTierValue,
                  normalizedVersionValue](QByteArray payload, QString error) mutable {
        if (!error.isEmpty()) return fallback(error);
        QString parseError;
        OpggBuild build = OpggCrawler::parseBuild(payload, &parseError);
        if (!build.isValid() || build.primaryStyleId <= 0 || build.runeIds.isEmpty()) return fallback(parseError);
        if (build.championId <= 0) build.championId = championId;
        if (build.position.isEmpty()) build.position = normalizedPositionValue;
        build.rankTier = normalizedRankTierValue;
        if (build.version.isEmpty()) build.version = normalizedVersionValue;
        // Ranked/flex/classic pages expose a meaningful lane split. Always
        // enrich those API responses; for mode pages without lanes the API
        // result is already the complete source.
        const bool hasLaneMetadata = normalizedModeValue == QStringLiteral("SOLORANKED")
            || normalizedModeValue == QStringLiteral("FLEXRANKED")
            || normalizedModeValue == QStringLiteral("NORMAL");
        if (hasLaneMetadata) return enrichPageMetadata(std::move(build));
        finishBuild(std::move(build), {});
    });
}

QList<OpggChampionRank> OpggCrawler::parseChampionRanking(const QByteArray &payload, QString *error)
{
    if (error) error->clear();
    const QList<QJsonObject> objects = embeddedObjects(payload);
    if (objects.isEmpty()) {
        if (error) *error = "OP.GG 返回内容无法解析为 JSON。";
        return {};
    }
    QList<OpggChampionRank> result;
    const QHash<QString, QString> localizedRoleLabels = localizedRoleLabelsFromMarkup(payload);
    const QString payloadVersion = versionFromPayload(payload);
    for (const QJsonObject &object : objects) {
        if (!isRankRecordObject(object)) continue;
        OpggChampionRank rank = rankFromObject(object, {});
        rank.version = payloadVersion;
        applyLocalizedRoleLabels(rank.roles, localizedRoleLabels);
        if (!likelyRankObject(rank)) continue;
        const auto duplicate = std::find_if(result.begin(), result.end(), [&rank](const OpggChampionRank &existing) {
            const bool sameChampion = rank.championId > 0 && existing.championId > 0
                ? rank.championId == existing.championId
                : (!rank.championKey.isEmpty() && !existing.championKey.isEmpty()
                    ? rank.championKey.compare(existing.championKey, Qt::CaseInsensitive) == 0
                    : !rank.championName.isEmpty()
                        && rank.championName.compare(existing.championName, Qt::CaseInsensitive) == 0);
            if (!sameChampion) return false;
            const QString leftPosition = normalizedPosition(rank.position);
            const QString rightPosition = normalizedPosition(existing.position);
            return leftPosition.isEmpty() || rightPosition.isEmpty() || leftPosition == rightPosition;
        });
        if (duplicate == result.end()) {
            result.append(rank);
        } else {
            // A page can emit the same champion once per class. Preserve all
            // OP.GG class labels while keeping one leaderboard row.
            for (const QString &role : rank.roles) appendUniqueRole(duplicate->roles, role);
            for (const OpggChampionReference &counter : rank.weakAgainst) {
                const bool exists = std::any_of(duplicate->weakAgainst.cbegin(), duplicate->weakAgainst.cend(),
                                                [&counter](const OpggChampionReference &current) {
                    return counter.championId > 0 && current.championId == counter.championId;
                });
                if (!exists) duplicate->weakAgainst.append(counter);
            }
            if (duplicate->version.isEmpty()) duplicate->version = rank.version;
        }
    }
    std::sort(result.begin(), result.end(), [](const OpggChampionRank &left, const OpggChampionRank &right) {
        if (left.rank > 0 && right.rank > 0 && left.rank != right.rank) return left.rank < right.rank;
        if (!qFuzzyCompare(left.winRate + 1.0, right.winRate + 1.0)) return left.winRate > right.winRate;
        return left.championName.localeAwareCompare(right.championName) < 0;
    });
    for (int index = 0; index < result.size(); ++index) if (result[index].rank <= 0) result[index].rank = index + 1;
    if (result.isEmpty() && error) *error = "OP.GG 返回内容中没有可识别的英雄排行。";
    return result;
}

OpggBuild OpggCrawler::parseBuild(const QByteArray &payload, QString *error)
{
    if (error) error->clear();
    const QList<QJsonObject> objects = embeddedObjects(payload);
    if (objects.isEmpty()) {
        if (error) *error = "OP.GG 返回内容无法解析为 JSON。";
        return {};
    }
    OpggBuild best;
    int bestScore = -1;
    QList<int> fallbackSpellIds;
    QList<OpggRuneBuild> runeBuilds;
    const QHash<QString, QString> localizedRoleLabels = localizedRoleLabelsFromMarkup(payload);
    const QString payloadVersion = versionFromPayload(payload);
    for (const QJsonObject &object : objects) {
        OpggBuild candidate = buildFromObject(object);
        applyLocalizedRoleLabels(candidate.roles, localizedRoleLabels);
        if (fallbackSpellIds.size() < 2) collectSpellIds(object, fallbackSpellIds);
        collectRuneBuildsFromValue(object, runeBuilds);
        const int score = candidate.runeIds.size() * 3 + candidate.itemIds.size()
            + candidate.summonerSpellIds.size() * 2 + (candidate.primaryStyleId > 0 ? 4 : 0);
        if (score > bestScore) {
            best = std::move(candidate);
            bestScore = score;
        }
    }
    best.version = payloadVersion;

    QList<OpggItemBuild> itemBuilds = itemBuildsFromFlightTexts(embeddedFlightTexts(payload));
    if (itemBuilds.isEmpty() && !best.itemIds.isEmpty()) {
        OpggItemBuild fallbackItems;
        fallbackItems.label = QStringLiteral("方案 1");
        fallbackItems.itemIds = best.itemIds;
        itemBuilds.append(std::move(fallbackItems));
    }

    const bool hasCompleteRuneBuild = std::any_of(runeBuilds.cbegin(), runeBuilds.cend(), [](const OpggRuneBuild &build) {
        return build.statShardIds.size() >= 3;
    });
    if (hasCompleteRuneBuild) {
        runeBuilds.erase(std::remove_if(runeBuilds.begin(), runeBuilds.end(), [](const OpggRuneBuild &build) {
            return build.statShardIds.size() < 3;
        }), runeBuilds.end());
    }
    std::stable_sort(runeBuilds.begin(), runeBuilds.end(), [](const OpggRuneBuild &left, const OpggRuneBuild &right) {
        if (!qFuzzyCompare(left.pickRate + 1.0, right.pickRate + 1.0)) return left.pickRate > right.pickRate;
        if (left.games != right.games) return left.games > right.games;
        return left.winRate > right.winRate;
    });
    for (int index = 0; index < runeBuilds.size(); ++index) {
        if (runeBuilds[index].label.trimmed().isEmpty()) {
            runeBuilds[index].label = QStringLiteral("方案 %1").arg(index + 1);
        }
    }
    best.runeBuilds = std::move(runeBuilds);
    best.itemBuilds = std::move(itemBuilds);

    // The page-level champion metadata and the rune rows are separate React
    // flight objects. Recover the identity from any object when the selected
    // build row itself does not carry it.
    if (best.championId <= 0 || best.championName.trimmed().isEmpty()
        || best.championKey.trimmed().isEmpty()) {
        const QString wantedKey = best.championKey.trimmed().toCaseFolded();
        const QString wantedName = best.championName.trimmed().toCaseFolded();
        int identityScore = -1;
        int selectedId = 0;
        QString selectedName;
        QString selectedKey;
        QStringList selectedRoles;
        for (const QJsonObject &object : objects) {
            const int id = championIdFrom(object);
            const QString name = championNameFrom(object);
            const QString key = championKeyFrom(object);
            if (id <= 0 && name.isEmpty() && key.isEmpty()) continue;

            const bool keyMatches = !wantedKey.isEmpty() && !key.isEmpty()
                && key.trimmed().compare(wantedKey, Qt::CaseInsensitive) == 0;
            // The build cards frequently use an English key while page-level
            // metadata is localized. Match the page route key as well as a
            // display name; `championKey` is the stable identity when present.
            const bool nameMatches = !wantedName.isEmpty() && !name.isEmpty()
                && name.trimmed().compare(wantedName, Qt::CaseInsensitive) == 0;
            if ((!wantedKey.isEmpty() || !wantedName.isEmpty()) && !keyMatches && !nameMatches) {
                // Once the selected build exposes an identity, numeric ids
                // from unrelated page widgets (items, navigation, or another
                // champion) are not valid candidates for this build.
                continue;
            }
            int score = id > 0 ? 100 : 0;
            if (keyMatches) score += 30;
            if (nameMatches) score += 20;
            if (object.contains(QStringLiteral("championId"))
                || object.contains(QStringLiteral("champion_id"))) score += 3;
            // Do not replace a key/name match with an unrelated metadata
            // object merely because it happens to contain a numeric id.
            if (!wantedKey.isEmpty() && !keyMatches && !nameMatches && id > 0) score -= 15;
            if (score <= identityScore) continue;

            identityScore = score;
            selectedId = id;
            selectedName = name;
            selectedKey = key;
            selectedRoles = opggRolesFromObject(object);
            applyLocalizedRoleLabels(selectedRoles, localizedRoleLabels);
        }
        if (identityScore >= 0) {
            if (selectedId > 0) best.championId = selectedId;
            if (best.championName.trimmed().isEmpty() && !selectedName.isEmpty()) best.championName = selectedName;
            if (best.championKey.trimmed().isEmpty() && !selectedKey.isEmpty()) best.championKey = selectedKey;
            for (const QString &role : selectedRoles) appendUniqueRole(best.roles, role);
        }
    }

    // Role metadata is emitted by several unrelated components on a build
    // page (rune cards, navigation and champion selectors).  Aggregating the
    // roles from every embedded object produces labels for other champions.
    // Keep only role records whose champion identity matches the selected
    // build. Identity-free page-header metadata is considered only after this
    // scoped pass; the global class-option list is never treated as a role.
    QStringList scopedRoles;
    const QString bestKey = best.championKey.trimmed().toCaseFolded();
    const QString bestName = best.championName.trimmed().toCaseFolded();
    const bool identityKnown = best.championId > 0 || !bestKey.isEmpty() || !bestName.isEmpty();
    for (const QJsonObject &object : objects) {
        OpggBuild candidate = buildFromObject(object);
        if (candidate.roles.isEmpty()) continue;
        const QString candidateKey = candidate.championKey.trimmed().toCaseFolded();
        const QString candidateName = candidate.championName.trimmed().toCaseFolded();
        const bool sameIdentity = (!bestKey.isEmpty() && candidateKey == bestKey)
            || (!bestName.isEmpty() && candidateName == bestName)
            || (best.championId > 0 && candidate.championId == best.championId);
        // With no selected-champion identity, defer to pageRolesFromObjects,
        // which scores one page-level record instead of aggregating roles from
        // every unrelated object in the flight stream.
        if (!identityKnown || !sameIdentity) continue;
        applyLocalizedRoleLabels(candidate.roles, localizedRoleLabels);
        for (const QString &role : candidate.roles) appendUniqueRole(scopedRoles, role);
    }
    // Prefer a page/header role record that is tied to this champion.  Do not
    // use every option in the global class selector as a fallback: that list
    // contains all possible classes and was the source of incorrect labels.
    const QStringList pageRoles = pageRolesFromObjects(objects, best, localizedRoleLabels);
    if (!scopedRoles.isEmpty()) {
        // Identity-scoped records are more authoritative than an identity-free
        // header component, which may be reused by navigation controls.
        best.roles = std::move(scopedRoles);
    } else if (!pageRoles.isEmpty()) {
        best.roles = pageRoles;
    }

    if (!best.runeBuilds.isEmpty()) {
        const OpggRuneBuild &preferred = best.runeBuilds.constFirst();
        best.primaryStyleId = preferred.primaryStyleId;
        best.subStyleId = preferred.subStyleId;
        if (best.runeIds.isEmpty() || best.runeIds.size() < preferred.runeIds.size()) {
            best.runeIds = preferred.runeIds;
        }
        best.statShardIds = preferred.statShardIds;
        if (!preferred.summonerSpellIds.isEmpty()) best.summonerSpellIds = preferred.summonerSpellIds;
    }
    if (!best.itemBuilds.isEmpty()) {
        const OpggItemBuild &preferred = best.itemBuilds.constFirst();
        best.itemIds = preferred.itemIds;
    }
    if (best.summonerSpellIds.isEmpty() && fallbackSpellIds.size() >= 2) {
        best.summonerSpellIds = fallbackSpellIds.mid(0, 2);
    }
    const PagePositionMetadata pagePositions = positionMetadataFromObjects(objects, best);
    if (!pagePositions.selectedPosition.isEmpty()) best.position = pagePositions.selectedPosition;
    best.laneStats = pagePositions.laneStats;
    if (best.laneStats.isEmpty()) best.laneStats = laneStatsFromMarkup(payload);
    if (best.position.isEmpty()) {
        const QRegularExpression routePattern(
            QStringLiteral("/build/(top|jungle|mid|adc|support)(?:[?\\\"'/]|$)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch route = routePattern.match(QString::fromUtf8(payload));
        if (route.hasMatch()) best.position = route.captured(1).toLower();
    }
    best.skillOrder = skillOrderFromMarkup(payload);
    best.spellBuilds = spellBuildsFromFlightTexts(embeddedFlightTexts(payload));
    if (best.spellBuilds.isEmpty() && best.summonerSpellIds.size() >= 2) {
        OpggSpellBuild fallbackSpells;
        fallbackSpells.spellIds = best.summonerSpellIds.mid(0, 2);
        best.spellBuilds.append(std::move(fallbackSpells));
    }
    if (!best.isValid() && error) *error = "OP.GG 返回内容中没有可识别的推荐配置。";
    return best;
}

QList<OpggChampionReference> OpggCrawler::parseCounterPage(const QByteArray &payload, QString *error)
{
    if (error) error->clear();
    QList<OpggChampionReference> result = parseCounterMarkup(payload);
    if (result.isEmpty()) {
        // API/flight payloads used by older OP.GG deployments expose the same
        // records as JSON.  Reuse the structured collector as a fallback.
        const QList<QJsonObject> objects = embeddedObjects(payload);
        for (const QJsonObject &object : objects) {
            for (const char *key : {"counters", "counterChampions", "counter_champions", "matchups",
                                    "weakAgainst", "strongAgainst", "data"}) {
                const QJsonValue value = object.value(QLatin1String(key));
                if (value.isArray() || value.isObject()) collectCounters(value, result);
            }
        }
    }
    if (result.isEmpty() && error) *error = QStringLiteral("OP.GG 未返回 Counter 数据。");
    return result;
}

} // namespace Janna
