#include "core/Rules.h"

#include "core/Files.h"
#include "core/Library.h"
#include "core/Tags.h"

#include <QJsonArray>

#include <array>

namespace core {

namespace {

/// How a condition kind is written in `rules.json`: the field it looks at and the comparison.
struct ConditionKey
{
    RuleCondition::Kind kind;
    QLatin1StringView field;
    QLatin1StringView op;
};

constexpr std::array ConditionKeys{
    ConditionKey{RuleCondition::Kind::TagEquals, QLatin1StringView("tag"),
                 QLatin1StringView("equals")},
    ConditionKey{RuleCondition::Kind::NoTags, QLatin1StringView("tags"), QLatin1StringView("none")},
};

/// The conditions of earlier versions, as field and comparison: on the title, and a tag
/// matching a regular expression. A rule with one of them is read as unsupported.
constexpr std::array LegacyConditionKeys{
    std::pair{QLatin1StringView("title"), QLatin1StringView("matches")},
    std::pair{QLatin1StringView("title"), QLatin1StringView("contains")},
    std::pair{QLatin1StringView("title"), QLatin1StringView("equals")},
    std::pair{QLatin1StringView("tag"), QLatin1StringView("matches")},
};

QJsonObject ruleToJson(const Rule &rule)
{
    QJsonObject object = rule.extra;
    object.insert(QStringLiteral("id"), rule.id);
    object.insert(QStringLiteral("name"), rule.name);
    object.insert(QStringLiteral("enabled"), rule.enabled);
    object.insert(QStringLiteral("match"),
                  rule.matchAll ? QStringLiteral("all") : QStringLiteral("any"));
    // An unsupported rule's conditions are among its unknown fields.
    if (rule.supported) {
        QJsonArray conditions;
        for (const RuleCondition &condition : rule.conditions) {
            const auto *const key =
                std::ranges::find(ConditionKeys, condition.kind, &ConditionKey::kind);
            QJsonObject entry{{QStringLiteral("field"), QString(key->field)},
                              {QStringLiteral("op"), QString(key->op)}};
            if (condition.kind != RuleCondition::Kind::NoTags) {
                entry.insert(QStringLiteral("value"), condition.value);
            }
            conditions.append(entry);
        }
        object.insert(QStringLiteral("conditions"), conditions);
    }
    object.insert(QStringLiteral("action"),
                  QJsonObject{{QStringLiteral("kind"), rule.action.kind == RuleAction::Kind::Move
                                                           ? QStringLiteral("move")
                                                           : QStringLiteral("add")},
                              {QStringLiteral("collections"),
                               QJsonArray::fromStringList(rule.action.collections)}});
    object.insert(QStringLiteral("stop"), rule.stop);
    object.insert(QStringLiteral("created"), formatTime(rule.created));
    object.insert(QStringLiteral("updated"), formatTime(rule.updated));
    return object;
}

/// Reads one rule; returns no value when it does not follow the format. `caseSensitive`, which
/// only the conditions of earlier versions used, is kept as an unknown field.
std::optional<Rule> ruleFromJson(const QJsonObject &entry)
{
    if (!entry.value(QStringLiteral("conditions")).isArray() ||
        !entry.value(QStringLiteral("action")).isObject() ||
        !entry.value(QStringLiteral("action"))
             .toObject()
             .value(QStringLiteral("collections"))
             .isArray()) {
        return std::nullopt;
    }
    for (const auto key : {QLatin1StringView("enabled"), QLatin1StringView("stop")}) {
        if (const QJsonValue value = entry.value(key); !value.isUndefined() && !value.isBool()) {
            return std::nullopt;
        }
    }
    Rule rule;
    rule.id = entry.value(QStringLiteral("id")).toString();
    rule.name = entry.value(QStringLiteral("name")).toString();
    rule.enabled = entry.value(QStringLiteral("enabled")).toBool(true);
    const QString match = entry.value(QStringLiteral("match")).toString();
    rule.matchAll = match != QLatin1StringView("any");
    rule.stop = entry.value(QStringLiteral("stop")).toBool(false);
    const auto created = parseTime(entry.value(QStringLiteral("created")).toString());
    const auto updated = parseTime(entry.value(QStringLiteral("updated")).toString());
    if (!isValidId(rule.id) || !created || !updated ||
        (match != QLatin1StringView("all") && match != QLatin1StringView("any"))) {
        return std::nullopt;
    }
    rule.created = *created;
    rule.updated = *updated;
    for (const QJsonValue value : entry.value(QStringLiteral("conditions")).toArray()) {
        const QJsonObject condition = value.toObject();
        const QString field = condition.value(QStringLiteral("field")).toString();
        const QString op = condition.value(QStringLiteral("op")).toString();
        const auto *const key = std::ranges::find_if(ConditionKeys, [&](const ConditionKey &known) {
            return known.field == field && known.op == op;
        });
        if (key != ConditionKeys.end()) {
            rule.conditions.append(
                {.kind = key->kind, .value = condition.value(QStringLiteral("value")).toString()});
        } else if (std::ranges::any_of(LegacyConditionKeys, [&](const auto &legacy) {
                       return legacy.first == field && legacy.second == op;
                   })) {
            rule.supported = false;
        } else {
            return std::nullopt;
        }
    }
    const QJsonObject action = entry.value(QStringLiteral("action")).toObject();
    const QString kind = action.value(QStringLiteral("kind")).toString();
    if (kind != QLatin1StringView("add") && kind != QLatin1StringView("move")) {
        return std::nullopt;
    }
    rule.action.kind =
        kind == QLatin1StringView("move") ? RuleAction::Kind::Move : RuleAction::Kind::Add;
    for (const QJsonValue collection : action.value(QStringLiteral("collections")).toArray()) {
        if (!collection.isString() || !isValidId(collection.toString())) {
            return std::nullopt;
        }
        rule.action.collections.append(collection.toString());
    }
    rule.extra = unknownFields(entry, {QLatin1StringView("id"), QLatin1StringView("name"),
                                       QLatin1StringView("enabled"), QLatin1StringView("match"),
                                       QLatin1StringView("conditions"), QLatin1StringView("action"),
                                       QLatin1StringView("stop"), QLatin1StringView("created"),
                                       QLatin1StringView("updated")});
    if (!rule.supported) {
        rule.conditions.clear();
        rule.extra.insert(QStringLiteral("conditions"), entry.value(QStringLiteral("conditions")));
    }
    return rule;
}

} // namespace

Result<QList<Rule>> parseRules(const QByteArray &data, const QString &path)
{
    auto object = parseJsonObject(data, path);
    if (!object) {
        return std::unexpected(object.error());
    }
    if (object->value(QStringLiteral("format")).toInt(0) > LibraryFormat) {
        return fail(ErrorCode::NewerFormat, path);
    }
    if (object->value(QStringLiteral("format")).toInt(0) < 1 ||
        !object->value(QStringLiteral("rules")).isArray()) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid rules document"));
    }
    QList<Rule> rules;
    for (const QJsonValue value : object->value(QStringLiteral("rules")).toArray()) {
        auto rule = ruleFromJson(value.toObject());
        if (!rule) {
            return fail(ErrorCode::Malformed, path,
                        QStringLiteral("invalid rule %1")
                            .arg(value.toObject().value(QStringLiteral("id")).toString()));
        }
        rules.append(std::move(*rule));
    }
    return rules;
}

QByteArray serializeRules(const QList<Rule> &rules)
{
    QJsonArray list;
    for (const Rule &rule : rules) {
        list.append(ruleToJson(rule));
    }
    return toJsonText(
        QJsonObject{{QStringLiteral("format"), LibraryFormat}, {QStringLiteral("rules"), list}});
}

bool RuleEngine::matches(qsizetype index, const QStringList &tags) const
{
    const Rule &rule = m_rules.at(index);
    // An unsupported rule has no conditions, so it never matches.
    if (rule.conditions.isEmpty()) {
        return false;
    }
    for (const RuleCondition &condition : rule.conditions) {
        bool hit = false;
        switch (condition.kind) {
        case RuleCondition::Kind::TagEquals: {
            const QString wanted = normalizedTag(condition.value);
            hit = !wanted.isEmpty() && std::ranges::any_of(tags, [&](const QString &tag) {
                return isTagWithin(tag, wanted);
            });
            break;
        }
        case RuleCondition::Kind::NoTags:
            hit = tags.isEmpty();
            break;
        }
        if (hit != rule.matchAll) {
            // All fails on the first miss, Any succeeds on the first hit.
            return hit;
        }
    }
    return rule.matchAll;
}

bool RuleEngine::apply(const std::optional<QStringList> &before, NoteMeta &meta,
                       const QList<Collection> &collections) const
{
    if (meta.archived || meta.trashed) {
        return false;
    }
    const QStringList original = meta.collections;
    for (qsizetype i = 0; i < m_rules.size(); ++i) {
        const Rule &rule = m_rules.at(i);
        if (!rule.enabled || !matches(i, meta.tags)) {
            continue;
        }
        // A rule acts only when the note starts matching it, so taking the note out of a
        // collection by hand is not undone by the next edit.
        if (!before || !matches(i, *before)) {
            act(rule, meta, collections);
        }
        if (rule.stop) {
            break;
        }
    }
    return meta.collections != original;
}

bool RuleEngine::act(const Rule &rule, NoteMeta &meta, const QList<Collection> &collections)
{
    QStringList targets;
    for (const QString &id : rule.action.collections) {
        if (std::ranges::any_of(
                collections, [&id](const Collection &collection) { return collection.id == id; })) {
            targets.append(id);
        }
    }
    if (targets.isEmpty()) {
        return false;
    }
    const QStringList original = meta.collections;
    if (rule.action.kind == RuleAction::Kind::Move) {
        meta.collections = {targets.first()};
    } else {
        for (const QString &target : targets) {
            if (!meta.collections.contains(target)) {
                meta.collections.append(target);
            }
        }
    }
    return meta.collections != original;
}

} // namespace core
