#ifndef ORGELEMENT_H
#define ORGELEMENT_H

#include <QCoreApplication>
#include <QSharedPointer>

#include "orgmodeparser_export.h"

namespace OrgMode {

/** @brief OrgElement represents an element of an org mode file.
 *
 * Example elements are headlines, lines of text, and other parts of an org file.
 * Elements are always bound by line breaks.
 */
class ORGMODEPARSER_EXPORT OrgElement
{
    Q_DECLARE_TR_FUNCTIONS(OrgElement)
public:
    typedef QSharedPointer<OrgElement> Pointer;
    typedef QList<Pointer> List;

    OrgElement();
    virtual ~OrgElement();
    bool isValid() const;

    List children() const;
    void addChild(const Pointer& child);
    void setChildren(const List& children);

    int level() const;
    void setLevel(int level);

    QString describe() const;

protected:
    virtual bool isElementValid() const = 0;
    virtual QString mnemonic() const = 0;
    virtual QString description() const = 0;

private:
    class Private;
    Private* d;
};

}

#endif // ORGELEMENT_H
