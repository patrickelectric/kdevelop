/***************************************************************************
 *   Copyright (C) 2007 by Alexander Dymo  <adymo@kdevelop.org>            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/
#ifndef ILANGUAGECONTROLLER_H
#define ILANGUAGECONTROLLER_H

#include <QList>
#include <QObject>

#include <kdevexport.h>

namespace KDevelop {

class ILanguage;

class KDEVPLATFORMINTERFACES_EXPORT ILanguageController: public QObject {
public:
    ILanguageController(QObject *parent = 0);

    /**@return the currently active languages loaded for the currently active file.
    The list is empty if the file's language is unsupported.*/
    virtual QList<ILanguage*>activeLanguages() = 0;
    /**@return the language for given @p name.*/
    virtual ILanguage* language(const QString &name) = 0;

    /**@return the aggregate code model for all loaded language supports.*/
    //virtual CodeModel codeModel() = 0;

    /**@return the code model for currenly loaded file
    (code model is empty if file is not loaded or not supported).*/
    //virtual CodeModel activeCodeModel() = 0;

};

}

#endif

//kate: space-indent on; indent-width 4; tab-width: 4; replace-tabs on;
