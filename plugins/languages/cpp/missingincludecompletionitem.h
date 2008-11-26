/*
   Copyright 2008 David Nolden <david.nolden.kdevelop@art-master.de>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#ifndef MISSINGINCLUDECOMPLETIONITEM_H
#define MISSINGINCLUDECOMPLETIONITEM_H

#include "completionitem.h"
#include "cppduchain/expressionevaluationresult.h"

class MissingIncludeCompletionItem : public KDevelop::CompletionTreeItem {
public:
  MissingIncludeCompletionItem(QString addedInclude, QString displayTextPrefix, KDevelop::IndexedDeclaration decl, int argumentHintDepth) : m_addedInclude(addedInclude), m_displayTextPrefix(displayTextPrefix), m_decl(decl), m_argumentHintDepth(argumentHintDepth) {
  }

  virtual QVariant data(const QModelIndex& index, int role, const KDevelop::CodeCompletionModel* model) const;

  virtual void execute(KTextEditor::Document* document, const KTextEditor::Range& word);

  virtual int inheritanceDepth() const;
  virtual int argumentHintDepth() const {
    return m_argumentHintDepth;
  }

  int m_argumentHintDepth;
  QString m_addedInclude, m_displayTextPrefix;
  KDevelop::IndexedDeclaration m_decl;
};

///DUChain must be locked
///@param displayTextPrefix may be needed so the created items pass a specific filtering in the completion-list
QList<KDevelop::CompletionTreeItemPointer> missingIncludeCompletionItems(QString expression, QString displayTextPrefix, Cpp::ExpressionEvaluationResult expressionResult, KDevelop::DUContext* context, int argumentHintDepth = 0, bool namespaceAllowed = false);

#endif