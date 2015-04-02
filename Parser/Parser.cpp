#include <QIODevice>
#include <QTextStream>
#include <QRegularExpression>
#include <QtDebug>


#include "Parser.h"
#include "OrgElement.h"
#include "OrgLine.h"
#include "FileAttributeLine.h"
#include "OrgFile.h"
#include "Headline.h"
#include "Attributes.h"
#include "Properties.h"
#include "ClockLine.h"
#include "CompletedClockLine.h"
#include "Exception.h"
#include "OrgFileContent.h"
#include "Drawer.h"
#include "DrawerEntry.h"
#include "PropertyDrawer.h"
#include "PropertyDrawerEntry.h"
#include "DrawerClosingEntry.h"

namespace OrgMode {

class Parser::Private {
public:
    typedef std::pair<OrgFile::Pointer, QSharedPointer<OrgFileContent>> ParseRunOutput;

    Private(Parser* parser)
        : parser_(parser)
    {}

    /** @brief First parse pass that provide the file-level attributes.
     * @return The parse results and the content for the second pass in a std::pair.
     */
    ParseRunOutput parseOrgFileFirstPass(const OrgFileContent::Pointer& content, const QString& filename) const;
    /** @brief Parse a sequence of top level elements, considering it as one file unit. */
    OrgFile::Pointer parseOrgFile(OrgFileContent::Pointer content, const QString& filename) const;

    OrgElement::Pointer parseOrgElement(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const;
    OrgElement::Pointer parseOrgLine(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const;
    OrgElement::Pointer parseClockLine(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const;
    OrgElement::Pointer parseFileAttributeLine(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const;
    OrgElement::Pointer parseDrawerLine(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const;

    Parser* parser_;
    OrgFile::Pointer firstPassResults_;

private:
    QRegularExpressionMatch headlineMatch(const QString& line) const;
    QDateTime parseTimeStamp(const QString& text) const;
};

Parser::Private::ParseRunOutput Parser::Private::parseOrgFileFirstPass(const OrgFileContent::Pointer &content,
                                                                       const QString &filename) const
{
    const QSharedPointer<OrgFileContent> output(new OrgFileContent);
    const OrgFile::Pointer file(new OrgFile);
    file->setFileName(filename);
    QStringList lines;
    while(!content->atEnd()) {
        if (const OrgElement::Pointer element = parseFileAttributeLine(file, content)) {
            file->addChild(element);
            lines.append(element->line());
        } else {
            lines.append(content->getLine());
        }
    }
    output->ungetLines(lines);
    return std::make_pair(file, output);
}

OrgFile::Pointer Parser::Private::parseOrgFile(OrgFileContent::Pointer content, const QString &filename) const
{
    auto file = OrgFile::Pointer(new OrgFile);
    file->setFileName(filename);
    while(!content->atEnd()) {
        file->addChild(parseOrgElement(file, content));
    }
    return file;
}

OrgElement::Pointer Parser::Private::parseOrgElement(const OrgElement::Pointer &parent,
                                                     const OrgFileContent::Pointer &content) const
{
    //Let's see, is it a headline?
    const QString line = content->getLine();
    auto const match = headlineMatch(line);
    if (match.hasMatch()) {
        //If so, is it at the same or a lower level than the current element?
        const QString structureMarker = match.captured(1);
        if (structureMarker.length() <= parent->level()) {
            //The matched element is at the same level as this element.
            //Stop and return, this element has been completely parsed:
            content->ungetLine(line);
            return OrgElement::Pointer(); // end recursing
        } else {
            //This is a new headline, parse it and it's children until another sibling or parent headline is discovered
            auto self = Headline::Pointer(new Headline(line, parent.data()));
            QString description = match.captured(2);
            static const QRegularExpression tagsMatch(QStringLiteral("^(.+)(\\s+):(.+):\\s*$"));
            auto const match = tagsMatch.match(description);
            if (match.hasMatch()) {
                //We have tags:
                const QStringList tagsList = match.captured(3).split(QLatin1Char(':'));
                Headline::Tags tags;
                std::copy(tagsList.begin(), tagsList.end(), std::inserter(tags, tags.begin()));
                self->setTags(tags);
                //Set description to the remainder of the headline:
                description = match.captured(1).trimmed();
            }
            self->setCaption(description);
            while(OrgElement::Pointer child = parseOrgElement(self, content)) {
                self->addChild(child);
            }
            return self;
        }
    } else {
        //Not a headline, parse it as a non-recursive element.
        content->ungetLine(line);
        if (const OrgElement::Pointer element = parseClockLine(parent, content)) {
            return element;
        } else if (const OrgElement::Pointer element = parseFileAttributeLine(parent, content)) {
            return element;
        } else if (const OrgElement::Pointer element = parseDrawerLine(parent, content)) {
            return element;
        } else {
            //Every line is an OrgLine, so this is the fallback:
            return parseOrgLine(parent, content);
        }
    }
}

OrgElement::Pointer Parser::Private::parseOrgLine(const OrgElement::Pointer &parent, const OrgFileContent::Pointer &content) const
{
    Q_UNUSED(parent);
    if (content->atEnd()) {
        return OrgElement::Pointer();
    }
    const QString text = content->getLine();
    const auto element = OrgElement::Pointer(new OrgLine(text, parent.data()));
    return element;
}

OrgElement::Pointer Parser::Private::parseClockLine(const OrgElement::Pointer& parent, const OrgFileContent::Pointer& content) const
{
    static const QRegularExpression clockLineOpeningStructure(QStringLiteral("^(\\s*)CLOCK:\\s*\\[([- A-Z a-z 0-9 :]+)\\](.*)$"));
    const QString line = content->getLine();
    auto const match = clockLineOpeningStructure.match(line);
    if (match.hasMatch()) {
        auto const startText = match.captured(2);
        const QDateTime start = parseTimeStamp(startText);
        if (start.isValid()) {
            static const QRegularExpression clockLineStructure(QStringLiteral("^--\\[([- A-Z a-z 0-9 :]+)\\]"));
            const QString remainder = match.captured(3);
            auto const fullmatch = clockLineStructure.match(remainder);
            //update regex, match the rest
            if (fullmatch.hasMatch()) {
                auto const endText = fullmatch.captured(1);
                const QDateTime end = parseTimeStamp(endText);
                if (end.isValid()) {
                    //Closed clock entry
                    auto self = CompletedClockLine::Pointer(new CompletedClockLine(line, parent.data()));
                    self->setStartTime(start);
                    self->setEndTime(end);
                    return self;
                }
            } else {
                //Incomplete clock entry
                auto self = ClockLine::Pointer(new ClockLine(line, parent.data()));
                self->setStartTime(start);
                return self;
            }
        }
    }
    content->ungetLine(line);
    return OrgElement::Pointer();
}

OrgElement::Pointer Parser::Private::parseFileAttributeLine(const OrgElement::Pointer &parent,
                                                            const OrgFileContent::Pointer &content) const
{
    static const QRegularExpression fileAttributeStructure(QStringLiteral("\\#\\+(.+):\\s+(.*)$"));
    const QString line = content->getLine();
    auto const match = fileAttributeStructure.match(line);
    if (match.hasMatch()) {
        const QString key = match.captured(1);
        const QString value = match.captured(2);
        if (!key.isEmpty()) {
            auto self = new FileAttributeLine(line, parent.data());
            self->setProperty(Property(key, value));
            return OrgElement::Pointer(self);
        }
    }
    content->ungetLine(line);
    return OrgElement::Pointer();
}

//TODO implement as traverse_depth_first(element, [](const OrgElement::Pointer&) {});
QStringList collectLines(const OrgElement::Pointer& element) {
    QStringList lines;
    lines << element->line();
    for(const OrgElement::Pointer child : element->children()) {
        lines << collectLines(child);
    }
    return lines;
}

OrgElement::Pointer Parser::Private::parseDrawerLine(const OrgElement::Pointer &parent,
                                                     const OrgFileContent::Pointer &content) const
{
    static const QRegularExpression drawerTitleStructure(QStringLiteral("^\\s+:(.+):\\s*(.*)$"));
    const QString line = content->getLine();
    auto const match = drawerTitleStructure.match(line);
    if (match.hasMatch()) {
        const QString name = match.captured(1);
        const QString value = match.captured(2);
        if (!value.isEmpty()) {
            content->ungetLine(line);
            return OrgElement::Pointer();
        }
        const Attributes attributes(firstPassResults_);
        const QStringList drawernames = attributes.drawerNames();
        if (drawernames.contains(name)) {
            //This is a drawer
            Drawer::Pointer self;
            if (name == QLatin1String("PROPERTIES")) {
                self.reset(new PropertyDrawer(line, parent.data()));
            } else {
                self.reset(new Drawer(line, parent.data()));
            }
            self->setName(name);
            //Parse elements until :END: (complete) or headline (cancel)
            static const QRegularExpression drawerEntryStructure(QStringLiteral("^\\s*:(.+):\\s*(.*)$"));
            while(!content->atEnd()) {
                const QString line = content->getLine();
                if (headlineMatch(line).hasMatch()) {
                    const QStringList lines = QStringList() << collectLines(self) << line;
                    content->ungetLines(lines);
                    return OrgElement::Pointer();
                }
                auto const drawerEntryMatch = drawerEntryStructure.match(line);
                if (drawerEntryMatch.hasMatch()) {
                    const QString name = drawerEntryMatch.captured(1);
                    const QString value = drawerEntryMatch.captured(2).trimmed();
                    if (name == QStringLiteral("END")) {
                        //The end element, add it to the drawer, return
                        const DrawerClosingEntry::Pointer child(new DrawerClosingEntry(line, self.data()));
                        child->setProperty(Property(name, value));
                        self->addChild(child);
                        break;
                    } else {
                        //This is a drawer entry, specifying one key-value pair
                        DrawerEntry::Pointer child;
                        Property property(name, value);
                        if (self.dynamicCast<PropertyDrawer>()) {
                            //Property drawer entries may extend existing values:
                            if (name.endsWith(QLatin1Char('+'))) {
                                property.setKey(name.mid(0, name.length() -1));
                                property.setOperation(Property::Property_Add);
                            }
                            child.reset(new PropertyDrawerEntry(line, self.data()));
                        } else {
                            child.reset(new DrawerEntry(line, self.data()));
                        }
                        child->setProperty(property);
                        self->addChild(child);
                    }
                } else {
                    //The line is not a drawer entry, but located within a drawer.
                    //Consider it a regular OrgLine.
                    self->addChild(OrgLine::Pointer(new OrgLine(line, self.data())));
                }
            }
            return self;
        } else {
           //This is just a regular line that looks like a drawer, do nothing
        }
    }
    content->ungetLine(line);
    return OrgElement::Pointer();
}

QRegularExpressionMatch Parser::Private::headlineMatch(const QString &line) const
{
    static const QRegularExpression beginningOfHeadline(QStringLiteral("^([*]+)\\s+(.*)$"));
    auto const match = beginningOfHeadline.match(line);
    return match;
}

namespace {

class ConversionException : public RuntimeException
{
public:
    ConversionException() : RuntimeException(QString()) {}
};

int qstring_toint(const QString& value) {
    bool ok;
    const int result = value.toInt(&ok);
    if (!ok) {
        throw ConversionException();
    }
    return result;
}

}

QDateTime Parser::Private::parseTimeStamp(const QString &text) const
{
    static const QString format = QString::fromLatin1("yyyy-MM-dd ddd hh:mm");
    static const int formatLength = format.length();
    //test is of format "yyyy-MM-dd ddd hh:mm"
    //Using QDateTime::fromString(text, "yyyy-MM-dd ddd hh:mm") causes repeated calls to libicu and is rather slow
    //Instead, the strings will be parsed directly:
    const QString input = text.trimmed(); // don't break on leading or trailing whitespace
    if (input.length() != formatLength) {
        return QDateTime();
    }
    try {
        const int year = qstring_toint(input.mid(0, 4));
        const int month = qstring_toint(input.mid(5, 2));
        const int day = qstring_toint(input.mid(8, 2));
        const int hours = qstring_toint(input.mid(15, 2));
        const int minutes = qstring_toint(input.mid(18, 2));
        const QTime time(hours, minutes);
        const QDate date(year, month, day);
        return QDateTime(date, time);
    } catch (const ConversionException&) {
        return QDateTime();
    }
}

Parser::Parser(QObject *parent)
    : QObject(parent)
    , d(new Private(this))
{
}

Parser::~Parser()
{
    delete d; d = 0;
}

OrgElement::Pointer Parser::parse(QTextStream *data, const QString &fileName) const
{
    Q_ASSERT(data);
    const OrgFileContent::Pointer content(new OrgFileContent(data));
    auto const firstPassResults = d->parseOrgFileFirstPass(content, fileName);
    d->firstPassResults_ = firstPassResults.first;
    return d->parseOrgFile(firstPassResults.second, fileName);
}

}
