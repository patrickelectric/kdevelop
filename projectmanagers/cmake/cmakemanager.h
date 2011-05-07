/* KDevelop CMake Support
 *
 * Copyright 2006 Matt Rogers <mattr@kde.org>
 * Copyright 2007-2009 Aleix Pol <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef CMAKEMANAGER_H
#define CMAKEMANAGER_H

#include <QList>
#include <QString>
#include <QtCore/QVariant>

#include <KDirWatch>

#include <project/interfaces/iprojectfilemanager.h>
#include <project/interfaces/ibuildsystemmanager.h>
#include <language/interfaces/ilanguagesupport.h>
#include <language/codegen/applychangeswidget.h>
#include <interfaces/iplugin.h>
#include <interfaces/idocumentationprovider.h>

#include "cmakelistsparser.h"
#include "icmakemanager.h"
#include "cmakeprojectvisitor.h"

struct CMakeProjectData;
class QStandardItem;
class QDir;
class QObject;
class CMakeHighlighting;
class CMakeDocumentation;

namespace KDevelop
{
    class IProject;
    class IProjectBuilder;
    class ILanguage;
    class ICodeHighlighting;
    class ProjectFolderItem;
    class ProjectBaseItem;
    class ProjectFileItem;
    class ProjectTargetItem;
    class ParseJob;
    class ContextMenuExtension;
    class Context;
}

class CMakeFolderItem;

class CMakeManager
    : public KDevelop::IPlugin
    , public KDevelop::IBuildSystemManager
    , public KDevelop::ILanguageSupport
    , public ICMakeManager
{
Q_OBJECT
Q_INTERFACES( KDevelop::IBuildSystemManager )
Q_INTERFACES( KDevelop::IProjectFileManager )
Q_INTERFACES( KDevelop::ILanguageSupport )
Q_INTERFACES( ICMakeManager )
public:
    explicit CMakeManager( QObject* parent = 0, const QVariantList& args = QVariantList() );

    virtual ~CMakeManager();
    virtual Features features() const { return Features(Folders | Targets | Files ); }
//     virtual KDevelop::IProject* project() const;
    virtual KDevelop::IProjectBuilder* builder(KDevelop::ProjectFolderItem*) const;
    virtual KUrl buildDirectory(KDevelop::ProjectBaseItem*) const;
    virtual KUrl::List includeDirectories(KDevelop::ProjectBaseItem *) const;
    virtual QHash<QString, QString> defines(KDevelop::ProjectBaseItem *) const;

    virtual KDevelop::ProjectFolderItem* addFolder( const KUrl& folder, KDevelop::ProjectFolderItem* parent );
    virtual KDevelop::ProjectFileItem* addFile( const KUrl&, KDevelop::ProjectFolderItem* );
    virtual KDevelop::ProjectTargetItem* createTarget( const QString&, KDevelop::ProjectFolderItem* ) { return 0; }
    virtual bool addFilesToTarget( QList<KDevelop::ProjectFileItem*> files, KDevelop::ProjectTargetItem* target);

    virtual bool removeTarget( KDevelop::ProjectTargetItem* ) { return false; }
    virtual bool removeFilesFromTargets( QList<KDevelop::ProjectFileItem*> files );
    virtual bool removeFilesAndFolders( QList<KDevelop::ProjectBaseItem*> items);

    virtual bool renameFile(KDevelop::ProjectFileItem*, const KUrl&);
    virtual bool renameFolder(KDevelop::ProjectFolderItem*, const KUrl&);
    virtual bool moveFilesAndFolders( QList< KDevelop::ProjectBaseItem* > items, KDevelop::ProjectFolderItem *newParent );

    QList<KDevelop::ProjectTargetItem*> targets() const;
    QList<KDevelop::ProjectTargetItem*> targets(KDevelop::ProjectFolderItem* folder) const;

    virtual QList<KDevelop::ProjectFolderItem*> parse( KDevelop::ProjectFolderItem* dom );
    virtual KDevelop::ProjectFolderItem* import( KDevelop::IProject *project );
    
    virtual bool reload(KDevelop::ProjectFolderItem*);

    KDevelop::ContextMenuExtension contextMenuExtension( KDevelop::Context* context );
    
    virtual QPair<QString, QString> cacheValue(KDevelop::IProject* project, const QString& id) const;
    
    //LanguageSupport
    virtual QString name() const;
    virtual KDevelop::ParseJob *createParseJob(const KUrl &url);
    virtual KDevelop::ILanguage *language();
    virtual KDevelop::ICodeHighlighting* codeHighlighting() const;
    virtual QWidget* specialLanguageObjectNavigationWidget(const KUrl& url, const KDevelop::SimpleCursor& position);

private slots:
    void dirtyFile(const QString& file);

    void jumpToDeclaration();
    void projectClosing(KDevelop::IProject*);
    void reimportDone(KJob* job);
    
    void deletedWatched(const QString& directory);

private:
    void reimport(KDevelop::ProjectFolderItem* fi);
    CacheValues readCache(const KUrl &path) const;
    bool isReloading(KDevelop::IProject* p);
    bool isCorrectFolder(const KUrl& url, KDevelop::IProject* p) const;
    
    QMutex m_reparsingMutex;
    QMutex m_busyProjectsMutex;
    KDevelop::ReferencedTopDUContext initializeProject(KDevelop::IProject* project);
    
    KDevelop::ReferencedTopDUContext includeScript(const QString& file, KDevelop::IProject * project, const QString& currentDir,
                                                    KDevelop::ReferencedTopDUContext parent);
    
    static void setTargetFiles(KDevelop::ProjectTargetItem* target, const KUrl::List& files);
    void reloadFiles(KDevelop::ProjectFolderItem* item);

    QMap<KDevelop::IProject*, CMakeProjectData> m_projectsData;
    QMap<KDevelop::IProject*, KDirWatch*> m_watchers;
    QMap<KUrl, CMakeFolderItem*> m_pending;
    
    QSet<KDevelop::IProject*> m_busyProjects;
    
    KDevelop::ICodeHighlighting *m_highlight;
    
    QList<KDevelop::ProjectBaseItem*> m_clickedItems;
};

#endif


