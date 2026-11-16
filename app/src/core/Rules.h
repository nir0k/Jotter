#pragma once

#include "core/Error.h"
#include "core/Note.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace core {

struct Collection;

/// A condition of a rule on a note's tags.
struct RuleCondition
{
    enum class Kind {
        /// The note has the tag or one of its children; tags compare case-insensitively.
        TagEquals,
        NoTags,
    };

    Kind kind = Kind::TagEquals;
    /// The tag; unused by NoTags.
    QString value;

    bool operator==(const RuleCondition &) const = default;
};

/// What a rule does with a note that starts matching it.
struct RuleAction
{
    enum class Kind {
        /// Adds the note to the collections, keeping its other memberships.
        Add,
        /// Moves the note into the first collection, removing it from all others.
        Move,
    };

    Kind kind = Kind::Add;
    QStringList collections;

    bool operator==(const RuleAction &) const = default;
};

/// A rule of `rules.json`: conditions joined by All or Any and an action.
struct Rule
{
    QString id;
    QString name;
    bool enabled = true;
    /// Whether all conditions must hold (All) or one is enough (Any).
    bool matchAll = true;
    /// Whether Jotter runs the rule: false for a rule with conditions of earlier versions (on
    /// the title or with a regular expression), which has no conditions here and keeps them in
    /// extra, written back unchanged. Such a rule never matches.
    bool supported = true;
    QList<RuleCondition> conditions;
    RuleAction action;
    /// Whether rules after this one are skipped for a note that matches it.
    bool stop = false;
    QDateTime created;
    QDateTime updated;
    /// Fields this build does not know, written back unchanged.
    QJsonObject extra;

    bool operator==(const Rule &) const = default;
};

/// Parses `rules.json`.
///
/// @param data The file's contents.
/// @param path The file, for errors.
/// @return The rules in their order, or Malformed / NewerFormat.
Result<QList<Rule>> parseRules(const QByteArray &data, const QString &path);

/// Serialises rules as the contents of `rules.json`.
QByteArray serializeRules(const QList<Rule> &rules);

/// Evaluates rules on notes.
class RuleEngine
{
public:
    RuleEngine() = default;
    explicit RuleEngine(QList<Rule> rules)
        : m_rules(std::move(rules))
    {}

    const QList<Rule> &rules() const { return m_rules; }

    /// Tells whether a note with @p tags matches the rule at @p index.
    bool matches(qsizetype index, const QStringList &tags) const;

    /// Runs the enabled rules in their order after a note was created or its tags changed. A
    /// rule acts only when the note starts matching it: a new note, or one that did not match
    /// before the change. A matching rule with Stop ends the run. Archived and trashed notes are
    /// left alone.
    ///
    /// @param before The tags before the change, or no value for a new note.
    /// @param meta The note after the change; its collections are updated.
    /// @param collections The collection tree; targets that no longer exist are skipped.
    /// @return Whether a rule changed the note's collections.
    bool apply(const std::optional<QStringList> &before, NoteMeta &meta,
               const QList<Collection> &collections) const;

    /// Carries out a rule's action on a note, as Apply to existing notes does.
    ///
    /// @return Whether the note's collections changed.
    static bool act(const Rule &rule, NoteMeta &meta, const QList<Collection> &collections);

private:
    QList<Rule> m_rules;
};

} // namespace core
