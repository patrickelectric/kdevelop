/*
   Copyright 2007 David Nolden <david.nolden.kdevelop@art-master.de>

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

#include "codecompletioncontext.h"
#include <ktexteditor/view.h>
#include <ktexteditor/document.h>
#include <klocalizedstring.h>
#include <language/duchain/ducontext.h>
#include <language/duchain/duchain.h>
#include <language/duchain/namespacealiasdeclaration.h>
#include <language/duchain/classfunctiondeclaration.h>
#include <language/duchain/functiondefinition.h>
#include <language/duchain/duchainlock.h>
#include <language/duchain/stringhelpers.h>
#include <language/duchain/safetycounter.h>
#include <language/interfaces/iproblem.h>
#include <util/pushvalue.h>
#include "cppduchain/cppduchain.h"
#include "cppduchain/typeutils.h"
#include "cppduchain/overloadresolution.h"
#include "cppduchain/viablefunctions.h"
#include "cppduchain/environmentmanager.h"
#include "cpptypes.h"
#include "stringhelpers.h"
#include "templatedeclaration.h"
#include "cpplanguagesupport.h"
#include "environmentmanager.h"
#include "cppduchain/cppduchain.h"
#include "cppdebughelper.h"
#include "missingincludecompletionitem.h"
#include <interfaces/idocumentcontroller.h>
#include "implementationhelperitem.h"
#include <qtfunctiondeclaration.h>
#include <language/duchain/duchainutils.h>
#include "missingincludecompletionmodel.h"
#include <templateparameterdeclaration.h>

// #define ifDebug(x) x

#define LOCKDUCHAIN     DUChainReadLocker lock(DUChain::lock())

QString last5Lines(QString str) {
  QStringList lines = str.split("\n");
  if(lines.count() < 5)
    return str;
  else
    return QStringList(lines.mid(lines.count()-5, 5)).join("\n");
}

///If this is enabled, KDevelop corrects wrong member access operators like "." on a pointer automatically
const bool assistAccessType = true;
///If this is enabled, no chain of useless argument-hints for binary operators is created.
const bool noMultipleBinaryOperators = true;
///Whether only items that are allowed to be accessed should be shown
const bool doAccessFiltering = true;
// #ifdef TEST_COMPLETION
// //Stub implementation that does nothing
// QList<KDevelop::CompletionTreeItemPointer> missingIncludeCompletionItems(QString /*expression*/, QString /*displayTextPrefix*/, Cpp::ExpressionEvaluationResult /*expressionResult*/, KDevelop::DUContext* /*context*/, int /*argumentHintDepth*/, bool /*needInstance*/) {
//   return QList<KDevelop::CompletionTreeItemPointer>();
// }
// #endif

QStringList binaryArithmeticOperators = QString("+ - * / % ^ & | < >" ).split( ' ', QString::SkipEmptyParts );

QStringList binaryModificationOperators = QString("+= -= *= /= %= ^= &= |= =" ).split( ' ', QString::SkipEmptyParts );

QStringList arithmeticComparisonOperators = QString("!= <= >= < >" ).split( ' ', QString::SkipEmptyParts );

QStringList allOperators = QString("++ + -- += -= *= /= %= ^= &= |= << >> >>= <<= == != <= >= && || [ - * / % & | = < >" ).split( ' ', QString::SkipEmptyParts );



//Whether the list of argument-hints should contain all overloaded versions of operators.
//Disabled for now, because there is usually a huge list of overloaded operators.
const int maxOverloadedOperatorArgumentHints = 5;
const int maxOverloadedArgumentHints = 5;

using namespace KDevelop;

namespace Cpp {

///@todo move these together with those from expressionvisitor into an own file, or make them unnecessary
QList<DeclarationPointer> convert( const QList<Declaration*>& list ) {
  QList<DeclarationPointer> ret;
  foreach( Declaration* decl, list )
    ret << DeclarationPointer(decl);
  return ret;
}

QList<Declaration*> convert( const QList<DeclarationPointer>& list ) {
  QList<Declaration*> ret;
  foreach( const DeclarationPointer &decl, list )
    if( decl )
      ret << decl.data();
  return ret;
}

QList<Declaration*> convert( const QList<DeclarationId>& decls, uint count, TopDUContext* top ) {

  QList<Declaration*> ret;
  for(int a = 0; a < count; ++a) {
    Declaration* d = decls[a].getDeclaration(top);
    if(d)
      ret << d;
  }

  return ret;
}

typedef PushValue<int> IntPusher;

///Extracts the last line from the given string
QString extractLastLine(const QString& str) {
  int prevLineEnd = str.lastIndexOf('\n');
  if(prevLineEnd != -1)
    return str.mid(prevLineEnd+1);
  else
    return str;
}

bool isPrefixKeyword(QString str) {
  return str == "new" || str == "return" || str == "else" || str == "throw" || str == "delete" || str == "emit";
}

int completionRecursionDepth = 0;

///Removes the given word from before the expression if it is there. Returns whether it was found + removed.
bool removePrefixWord(QString& expression, QString word) {
  if(expression.left(word.size()+1).trimmed() == word) {
    if(expression.size() >= word.size()+1)
      expression = expression.mid(word.size()+1);
    else
      expression.clear();
    return true;
  }
  return false;
}

CodeCompletionContext::CodeCompletionContext(DUContextPointer context, const QString& text, const QString& followingText, int depth, const QStringList& knownArgumentExpressions, int line ) : KDevelop::CodeCompletionContext(context, text, depth), m_memberAccessOperation(NoMemberAccess), m_knownArgumentExpressions(knownArgumentExpressions), m_contextType(Normal), m_onlyShowTypes(false), m_onlyShowSignals(false), m_onlyShowSlots(false), m_pointerConversionsBeforeMatching(0)
{
#ifndef TEST_COMPLETION  
  MissingIncludeCompletionModel::self().stop();
#endif
  
  if(m_duContext) {
    LOCKDUCHAIN;
    if(!m_duContext)
      return;
    
    if((m_duContext->type() == DUContext::Class || m_duContext->type() == DUContext::Namespace || m_duContext->type() == DUContext::Global))
      m_onlyShowTypes = true;

    Declaration* classDecl = Cpp::localClassFromCodeContext(m_duContext.data());
    if(classDecl) {
      kDebug() << "local class:" << classDecl->qualifiedIdentifier().toString(  );
      m_localClass = DUContextPointer(classDecl->internalContext());
    }
  }

  m_followingText = followingText;

  if(depth > 10) {
    kDebug() << "too much recursion";
    m_valid = false;
    return;
  }

  {
    //Since include-file completion has nothing in common with the other completion-types, process it within a separate function
    QString lineText = extractLastLine(m_text).trimmed();
    if(lineText.startsWith("#include")) {
      processIncludeDirective(lineText);
      return;
    }
  }

  m_valid = isValidPosition();
  if( !m_valid ) {
    log( "position not valid for code-completion" );
    return;
  }

//   ifDebug( log( "non-processed text: " + m_text ); )
  if(depth == 0) {
    preprocessText( line );
    m_text = clearComments( m_text );
    m_text = clearStrings( m_text );
  }
    
   m_text = stripFinalWhitespace( m_text );
   m_text = last5Lines(m_text);

  ifDebug( log( QString("depth %1").arg(depth) + " end of processed text: " + m_text ); )

  ///@todo template-parameters

  ///First: find out what kind of completion we are dealing with

  if( (m_text.endsWith( ':' ) && !m_text.endsWith("::")) || m_text.endsWith( ';' ) || m_text.endsWith('}') || m_text.endsWith('{') || m_text.endsWith(')') ) {
    ///We're at the beginning of a new statement. General completion is valid.
    return;
  }

    if( m_text.endsWith('.') ) {
    m_memberAccessOperation = MemberAccess;
    m_text = m_text.left( m_text.length()-1 );
  }

  if( m_text.endsWith("->") ) {
    m_memberAccessOperation = ArrowMemberAccess;
    m_text = m_text.left( m_text.length()-2 );
  }

  if( m_text.endsWith("::") ) {
    m_memberAccessOperation = StaticMemberChoose; //We need to decide later whether it is a MemberChoose
    m_text = m_text.left( m_text.length()-2 );
  }

  if( m_text.endsWith('(') || m_text.endsWith( '<' ) ) {
    if( depth == 0 ) {
      //The first context should never be a function-call context, so make this a NoMemberAccess context and the parent a function-call context.
      m_parentContext = new CodeCompletionContext( m_duContext, m_text, QString(), depth+1 );
      m_text.clear();
    }else{
      ExpressionParser expressionParser;
      
      if(m_text.endsWith('(')) {
        m_contextType = FunctionCall;
        m_memberAccessOperation = FunctionCallAccess;
        m_text = m_text.left( m_text.length()-1 );
      }else{
        //We need to check here whether this really is a template access, or whether it is a "smaller" operator,
        //which is handled below
        int start_expr = Utils::expressionAt( m_text, m_text.length()-1 );

        QString expr = m_text.mid(start_expr, m_text.length() - start_expr - 1).trimmed();        
        
        Cpp::ExpressionEvaluationResult result = expressionParser.evaluateExpression(expr.toUtf8(), m_duContext);
        if(!result.isValid() || (!result.isInstance || result.type.type().cast<FunctionType>())) {
          m_memberAccessOperation = TemplateAccess;
          m_text = m_text.left( m_text.length()-1 );
        }
      }

      ///Compute the types of the argument-expressions

      for( QStringList::const_iterator it = m_knownArgumentExpressions.begin(); it != m_knownArgumentExpressions.end(); ++it )
        m_knownArgumentTypes << expressionParser.evaluateExpression( (*it).toUtf8(), m_duContext );
    }
  }

  if( endsWithOperator( m_text ) && (m_memberAccessOperation != StaticMemberChoose || !m_text.trimmed().endsWith(">"))) {
    if( depth == 0 ) {
      //The first context should never be a function-call context, so make this a NoMemberAccess context and the parent a function-call context.
      m_parentContext = new CodeCompletionContext( m_duContext, m_text, QString(), depth+1 );
      m_text.clear();
    }else{
      m_memberAccessOperation = FunctionCallAccess;
      m_contextType = BinaryOperatorFunctionCall;
      m_operator = getEndFunctionOperator(m_text);
      m_text = m_text.left( m_text.length() - getEndOperator(m_text).length() );
    }
  }

  ///Eventually take preceding "*" and/or "&" operators and use them for pointer depth conversion of the completion items
  while(parentContext() && parentContext()->m_memberAccessOperation == FunctionCallAccess && parentContext()->m_contextType == BinaryOperatorFunctionCall && parentContext()->m_expression.isEmpty()) {
    if(parentContext()->m_operator == "*") {
      --m_pointerConversionsBeforeMatching;
      setParentContext(parentContext()->m_parentContext);
      continue;
    }
    if(parentContext()->m_operator == "&") {
      ++m_pointerConversionsBeforeMatching;
      setParentContext(parentContext()->m_parentContext);
      continue;
    }
    break;
  }
  
  ///Now find out where the expression starts

  /**
   * Possible cases:
   * a = exp; - partially handled
   * ...
   * return exp;
   * emit exp;
   * throw exp;
   * new Class;
   * delete exp;
   * a=function(exp
   * a = exp(
   * ClassType instance(
   *
   * What else?
   *
   * When the left and right part are only separated by a whitespace,
   * expressionAt returns both sides
   * */

  int start_expr = Utils::expressionAt( m_text, m_text.length() );

  m_expression = m_text.mid(start_expr).trimmed();
  ifDebug( log( "expression: " + m_expression ); )

  if(m_expression == "else")
    m_expression = QString();
  
  if(m_expression == "const" || m_expression == "typedef") {
    //The cursor is behind a "const .."
    m_onlyShowTypes = true;
    m_expression = QString();
  }
  
  if(m_expression == "emit")  {
    m_onlyShowSignals = true;
    m_expression = QString();
  }

  QString expressionPrefix = stripFinalWhitespace( m_text.left(start_expr) );

  ifDebug( log( "expressionPrefix: " + expressionPrefix ); )

  ///Handle constructions like "ClassType instance("
  if(!expressionPrefix.isEmpty() && (expressionPrefix.endsWith('>') || expressionPrefix[expressionPrefix.length()-1].isLetterOrNumber() || expressionPrefix[expressionPrefix.length()-1] == '_')) {
    int newExpressionStart = Utils::expressionAt(expressionPrefix, expressionPrefix.length());
    if(newExpressionStart > 0) {
      QString newExpression = expressionPrefix.mid(newExpressionStart).trimmed();
      QString newExpressionPrefix = stripFinalWhitespace( expressionPrefix.left(newExpressionStart) );
      if(!isPrefixKeyword(newExpression)) {
        if(newExpressionPrefix.isEmpty() || newExpressionPrefix.endsWith(';') || newExpressionPrefix.endsWith('{') || newExpressionPrefix.endsWith('}')) {
          kDebug(9007) << "skipping expression" << m_expression << "and setting new expression" << newExpression;
          m_expression = newExpression;
          expressionPrefix = newExpressionPrefix;
        }
      }
    }
  }

  ///Handle recursive contexts(Example: "ret = function1(param1, function2(" )
  if( expressionPrefix.endsWith( '<' ) || expressionPrefix.endsWith('(') || expressionPrefix.endsWith(',') ) {
    log( QString("Recursive function-call: Searching parent-context in \"%1\"").arg(expressionPrefix) );
    //Our expression is within a function-call. We need to find out the possible argument-types we need to match, and show an argument-hint.

    //Find out which argument-number this expression is, and compute the beginning of the parent function-call(parentContextLast)
    QStringList otherArguments;
    int parentContextEnd = expressionPrefix.length();

    skipFunctionArguments( expressionPrefix, otherArguments, parentContextEnd );

    QString parentContextText = expressionPrefix.left(parentContextEnd);

    log( QString("This argument-number: %1 Building parent-context from \"%2\"").arg(otherArguments.size()).arg(parentContextText) );
    m_parentContext = new CodeCompletionContext( m_duContext, parentContextText, QString(), depth+1, otherArguments );
  }

  ///Handle signal/slot access
  if(depth == 0 && m_parentContext && parentContext()->memberAccessOperation() == FunctionCallAccess) {
    LOCKDUCHAIN;
    if(!m_duContext)
      return;

    if(parentContext()->functionName() == "SIGNAL" || parentContext()->functionName() == "SLOT") {
      if(parentContext()->functionName() == "SIGNAL")
        m_onlyShowSignals = true;
      if(parentContext()->functionName() == "SLOT") {
        m_onlyShowSlots = true;
      }
      
      setParentContext(KSharedPtr<KDevelop::CodeCompletionContext>(parentContext()->parentContext()));
    }

    if(m_parentContext && parentContext()->memberAccessOperation() == FunctionCallAccess) {
      foreach(const Cpp::OverloadResolutionFunction &function, parentContext()->functions()) {
        if(function.function.declaration() && (function.function.declaration()->qualifiedIdentifier().toString() == "QObject::connect" || function.function.declaration()->qualifiedIdentifier().toString() == "QObject::disconnect")) {
          FunctionType::Ptr funType = function.function.declaration()->type<FunctionType>();
          if(funType && funType->arguments().size() > function.matchedArguments) {
            if(function.matchedArguments == 1 && parentContext()->m_knownArgumentTypes.size() >= 1) {
              ///Pick a signal from the class pointed to in the earlier element
              m_memberAccessOperation = SignalAccess;
            }else if(funType->arguments()[function.matchedArguments] && funType->arguments()[function.matchedArguments]->toString() == "const char*") {
              m_memberAccessOperation = SlotAccess;

              if(parentContext()->m_knownArgumentExpressions.size() > 1) {
                QString connectedSignal = parentContext()->m_knownArgumentExpressions[1];
                if(connectedSignal.startsWith("SIGNAL") && connectedSignal.endsWith(")") && connectedSignal.length() > 8) {
                  connectedSignal = connectedSignal.mid(7);
                  connectedSignal = connectedSignal.left(connectedSignal.length()-1);
                  //Now connectedSignal is something like myFunction(...), and we want the "...".
                  QPair<Identifier, QByteArray> signature = Cpp::qtFunctionSignature(connectedSignal.toUtf8());
                  m_connectedSignalIdentifier = signature.first;
                  m_connectedSignalNormalizedSignature = signature.second;
                }
              }
            }

            if(m_memberAccessOperation == SignalAccess || m_memberAccessOperation == SlotAccess) {
              if(function.matchedArguments == 2) {
                //The function that does not take the target-argument is being used
                if(Declaration* klass = Cpp::localClassFromCodeContext(m_duContext.data()))
                  m_expressionResult.type = klass->indexedType();
              }else{
                m_expressionResult = parentContext()->m_knownArgumentTypes[function.matchedArguments-1];
                m_expressionResult.type = TypeUtils::targetType(TypeUtils::matchingClassPointer(funType->arguments()[function.matchedArguments-1], m_expressionResult.type.type(), m_duContext->topContext()), m_duContext->topContext())->indexed();
              }
              break;
            }
          }
        }
      }
    }
  }
  
  if(depth == 0 && parentContext() && parentContext()->memberAccessOperation() == TemplateAccess) {
    LOCKDUCHAIN;
    //This also happens in cases like "for(int a = 0; a < |", so test whether the previous expression is an instance.
    if(parentContext()->m_expressionResult.isValid() && !parentContext()->m_expressionResult.isInstance)
      m_onlyShowTypes = true;
  }

  ///Handle overridden binary operator-functions
  if( endsWithOperator(expressionPrefix) || expressionPrefix.endsWith("return") ) {
    log( QString( "Recursive operator: creating parent-context with \"%1\"" ).arg(expressionPrefix) );
    m_parentContext = new CodeCompletionContext( m_duContext, expressionPrefix, QString(), depth+1 );
  }

  ///Now care about m_expression. It may still contain keywords like "new "

  bool isThrow = false;

  QString expr = m_expression.trimmed();

  removePrefixWord(expr, "emit");
  
  if( removePrefixWord(expr, "return") )  {
    if(!expr.isEmpty() || depth == 0) {
      //Create a new context for the "return"
      m_parentContext = new CodeCompletionContext( m_duContext, "return", QString(), depth+1 );
    }else{
      m_memberAccessOperation = ReturnAccess;
    }
  }
  if( removePrefixWord(expr, "delete") )  {
    QRegExp bracketRE("^\\s*\\[\\s*\\]");
    if (expr.contains(bracketRE))
      expr = expr.remove(bracketRE);
    
    if(!expr.isEmpty() || depth == 0) {
      //Create a new context for the "delete"
      // TODO add brackets if necessary?
      m_parentContext = new CodeCompletionContext( m_duContext, "delete", QString(), depth+1 );
    }else{
      //m_memberAccessOperation = DeleteAccess;
    }
  }
  if( removePrefixWord(expr, "throw") )  {
    isThrow = true;
  }
  if( removePrefixWord(expr, "new") ) {
    m_onlyShowTypes = true;
    m_pointerConversionsBeforeMatching = 1;
  }
  ExpressionParser expressionParser/*(false, true)*/;

  ifDebug( kDebug(9007) << "expression: " << expr; )

  if( !expr.trimmed().isEmpty() && !m_expressionResult.isValid() ) {
    m_expressionResult = expressionParser.evaluateExpression( expr.toUtf8(), m_duContext );
    ifDebug( kDebug(9007) << "expression result: " << m_expressionResult.toString(); )
    if( !m_expressionResult.isValid() ) {
      if( m_memberAccessOperation != StaticMemberChoose ) {
        log( QString("expression \"%1\" could not be evaluated").arg(expr) );
        if(m_memberAccessOperation == FunctionCallAccess || m_memberAccessOperation == TemplateAccess)
          m_functionName = m_expression; //Keep the context valid, so missing-include completion can happen
        else
          m_valid = false;
          
        return;
      } else {
        //It may be an access to a namespace, like "MyNamespace::".
        //The "MyNamespace" can not be evaluated, still we can give some completions.
        return;
      }
    }
  }

  switch( m_memberAccessOperation ) {

    case NoMemberAccess:
    {
      if( !expr.trimmed().isEmpty() ) {
        //This should never happen, because the position-cursor should be chosen at the beginning of a possible completion-context(not in the middle of a string)
        log( QString("Cannot complete \"%1\" because there is an expression without an access-operation" ).arg(expr) );
        m_valid  = false;
      } else {
        //Do nothing. We do not have a completion-container, which means that a global completion should be done.
      }
    }
    break;
    case ArrowMemberAccess:
    {
      LOCKDUCHAIN;
      if(!m_duContext)
        return;
      
      //Dereference a pointer
      AbstractType::Ptr containerType = m_expressionResult.type.type();
      PointerType::Ptr pnt = TypeUtils::realType(containerType, m_duContext->topContext()).cast<PointerType>();
      if( !pnt ) {
        AbstractType::Ptr realContainer = TypeUtils::realType(containerType, m_duContext->topContext());
        IdentifiedType* idType = dynamic_cast<IdentifiedType*>(realContainer.unsafeData());
        if( idType ) {
          Declaration* idDecl = idType->declaration(m_duContext->topContext());
          if( idDecl && idDecl->internalContext() ) {
            bool declarationIsConst = false;
            if (containerType->modifiers() & AbstractType::ConstModifier || idDecl->abstractType()->modifiers() & AbstractType::ConstModifier)
              declarationIsConst = true;

            QList<Declaration*> operatorDeclarations = Cpp::findLocalDeclarations(idDecl->internalContext(), Identifier("operator->"), m_duContext->topContext());
            if( !operatorDeclarations.isEmpty() ) {
              // TODO use Cpp::isAccessible on operator functions for more correctness?
              foreach(Declaration* decl, operatorDeclarations)
                m_expressionResult.allDeclarationsList().append(decl->id());
              
              FunctionType::Ptr function;
              foreach (Declaration* decl, operatorDeclarations) {
                FunctionType::Ptr f2 = decl->abstractType().cast<FunctionType>();
                const bool operatorIsConst = f2->modifiers() & AbstractType::ConstModifier;
                if (operatorIsConst == declarationIsConst) {
                  // Best match
                  function = f2;
                  break;
                } else if (operatorIsConst && !function) {
                  // Const result where non-const is ok, accept and keep looking
                  function = f2;
                }
              }

              if( function ) {
                m_expressionResult.type = function->returnType()->indexed();
                m_expressionResult.isInstance = true;
              } else {
                  log( QString("arrow-operator of class is not a function, or is non-const where the object being accessed is const: %1").arg(containerType ? containerType->toString() : QString("null") ) );
              }
            } else {
              log( QString("arrow-operator on type without operator-> member: %1").arg(containerType ? containerType->toString() : QString("null") ) );
              if(idDecl->internalContext()->type() == DUContext::Class)
                replaceCurrentAccess("->", ".");
            }
          } else {
            log( QString("arrow-operator on type without declaration and context: %1").arg(containerType ? containerType->toString() : QString("null") ) );
          }
        } else {
          log( QString("arrow-operator on invalid type: %1").arg(containerType ? containerType->toString() : QString("null") ) );
          m_expressionResult = ExpressionEvaluationResult();
        }
      }

      if( pnt ) {
        ///@todo what about const in pointer?
        m_expressionResult.type = pnt->baseType()->indexed();
        m_expressionResult.isInstance = true;
      }
    }
    case MemberChoose:
    case StaticMemberChoose:
    case MemberAccess:
    {
      if( expr.trimmed().isEmpty() ) {
        log( "Expression was empty, cannot complete" );
        m_valid = false;
      }

      //The result of the expression is stored in m_expressionResult, so we're fine

      ///Additional step: Check whether we're accessing a declaration that is not available, and eventually allow automatically adding an #include
      LOCKDUCHAIN;
      if(!m_duContext)
        return;
      
      AbstractType::Ptr type = m_expressionResult.type.type();
      if(type && m_duContext) {
        DelayedType::Ptr delayed = type.cast<DelayedType>();
#ifndef TEST_COMPLETION // hmzzz ?? :)
        if(delayed && delayed->kind() == DelayedType::Unresolved) {
          eventuallyAddGroup(i18n("Not Included"), 1000, missingIncludeCompletionItems(m_expression, m_followingText.trimmed() + ": ", m_expressionResult, m_duContext.data(), 0, true));
        }
#endif
        if(type.cast<PointerType>())
          replaceCurrentAccess(".", "->");
      }else{
        log( "No type for expression" );
      }
    }
    break;
    case FunctionCallAccess:
      processFunctionCallAccess();
    break;
    case TemplateAccess:
      //Nothing to do for now
    break;
  }
}

CodeCompletionContext::AdditionalContextType CodeCompletionContext::additionalContextType() const {
  return m_contextType;
}

void CodeCompletionContext::processFunctionCallAccess() {
  ///Generate a list of all found functions/operators, together with each a list of optional prefixed parameters

  ///All the variable argument-count management in the following code is done to treat global operator-functions equivalently to local ones. Those take an additional first argument.

  LOCKDUCHAIN;
  if(!m_duContext)
    return;

  OverloadResolutionHelper helper( m_duContext, TopDUContextPointer(m_duContext->topContext()) );

  if( m_contextType == BinaryOperatorFunctionCall ) {

    if( !m_expressionResult.isInstance ) {
      log( "tried to apply an operator to a non-instance: " + m_expressionResult.toString() );
      m_valid = false;
      return;
    }

    helper.setOperator(OverloadResolver::Parameter(m_expressionResult.type.type(), m_expressionResult.isLValue()), m_operator);

    m_functionName = "operator"+m_operator;
  } else {
    ///Simply take all the declarations that were found by the expression-parser

    helper.setFunctions(convert(m_expressionResult.allDeclarations, m_expressionResult.allDeclarationsSize(), m_duContext->topContext()));

    if(m_expressionResult.allDeclarationsSize()) {
      Declaration* decl = m_expressionResult.allDeclarations[0].getDeclaration(m_duContext->topContext());
      if(decl)
        m_functionName = decl->identifier().toString();
    }
  }

  OverloadResolver::ParameterList knownParameters;
  foreach( const ExpressionEvaluationResult &result, m_knownArgumentTypes )
    knownParameters.parameters << OverloadResolver::Parameter( result.type.type(), result.isLValue() );

  helper.setKnownParameters(knownParameters);

  m_functions = helper.resolve(true);

//   if( declarations.isEmpty() ) {
//     log( QString("no list of function-declarations was computed for expression \"%1\"").arg(m_expression) );
//     return;
//   }
}

void CodeCompletionContext::processIncludeDirective(QString line)
{
  if(line.count('"') == 2 || line.endsWith('>'))
    return; //We are behind a complete include-directive

  kDebug(9007) << "include line: " << line;
  line = line.mid(8).trimmed(); //Strip away the #include
  kDebug(9007) << "trimmed include line: " << line;

  if(!line.startsWith('<') && !line.startsWith('"'))
    return; //We are not behind the beginning of a path-specification

  bool local = false;
  if(line.startsWith('"'))
    local = true;

  line = line.mid(1);

  kDebug(9007) << "extract prefix from " << line;
  //Extract the prefix-path
  KUrl u(line);
  u.setFileName(QString());
  QString prefixPath = u.path();
  kDebug(9007) << "extracted prefix " << prefixPath;

  LOCKDUCHAIN;
  if(!m_duContext)
    return;
#ifndef TEST_COMPLETION
  m_includeItems = CppLanguageSupport::self()->allFilesInIncludePath(KUrl(m_duContext->url().str()), local, prefixPath);
#endif
  m_valid = true;
  m_memberAccessOperation = IncludeListAccess;
}

const CodeCompletionContext::FunctionList& CodeCompletionContext::functions() const {
  return m_functions;
}

QString CodeCompletionContext::functionName() const {
  return m_functionName;
}

QList<Cpp::IncludeItem> CodeCompletionContext::includeItems() const {
  return m_includeItems;
}

ExpressionEvaluationResult CodeCompletionContext::memberAccessContainer() const {
  return m_expressionResult;
}

QList<DUContext*> CodeCompletionContext::memberAccessContainers() const {
  QList<DUContext*> ret;

  if( memberAccessOperation() == StaticMemberChoose && m_duContext ) {
    //Locate all namespace-instances we will be completing from
    ret += m_duContext->findContexts(DUContext::Class, QualifiedIdentifier(m_expression));
    ret += m_duContext->findContexts(DUContext::Namespace, QualifiedIdentifier(m_expression)); ///@todo respect position
  }

  if(m_expressionResult.isValid() ) {
    AbstractType::Ptr expressionTarget = TypeUtils::targetType(m_expressionResult.type.type(), m_duContext->topContext());
    const IdentifiedType* idType = dynamic_cast<const IdentifiedType*>( expressionTarget.unsafeData() );
      Declaration* idDecl = 0;
    if( idType && (idDecl = idType->declaration(m_duContext->topContext())) ) {
      DUContext* ctx = idDecl->logicalInternalContext(m_duContext->topContext());
      if( ctx ){
        if(ctx->type() != DUContext::Template) //Forward-declared template classes have a template-context assigned. Those should not be searched.
          ret << ctx;
      }else {
        //Print some debug-output
        kDebug(9007) << "Could not get internal context from" << m_expressionResult.type.type()->toString();
        kDebug(9007) << "Declaration" << idDecl->toString() << idDecl->isForwardDeclaration();
        if( Cpp::TemplateDeclaration* tempDeclaration = dynamic_cast<Cpp::TemplateDeclaration*>(idDecl) ) {
          if( tempDeclaration->instantiatedFrom() ) {
            kDebug(9007) << "instantiated from" << dynamic_cast<Declaration*>(tempDeclaration->instantiatedFrom())->toString() << dynamic_cast<Declaration*>(tempDeclaration->instantiatedFrom())->isForwardDeclaration();
            kDebug(9007) << "internal context" << dynamic_cast<Declaration*>(tempDeclaration->instantiatedFrom())->internalContext();
          }
        }

      }
    }
  }
  
//   foreach(DUContext* context, ret) {
//     kDebug() << "member-access container:" << context->url().str() << context->range().textRange() << context->scopeIdentifier(true).toString();
//   }

  return ret;
}

KDevelop::IndexedType CodeCompletionContext::applyPointerConversionForMatching(KDevelop::IndexedType type, bool fromLValue) const {
  if(m_pointerConversionsBeforeMatching == 0)
    return type;
  AbstractType::Ptr t = type.type();
  if(!t)
    return KDevelop::IndexedType();

  //Can only take addresses of lvalues
  if(m_pointerConversionsBeforeMatching > 1 || (m_pointerConversionsBeforeMatching && !fromLValue))
    return IndexedType();
  
  if(m_pointerConversionsBeforeMatching > 0) {
    for(int a = 0; a < m_pointerConversionsBeforeMatching; ++a) {
      
      t = TypeUtils::increasePointerDepth(t);
      if(!t)
        return IndexedType();
    }
  }else{
    for(int a = m_pointerConversionsBeforeMatching; a < 0; ++a) {
      t = TypeUtils::decreasePointerDepth(t, m_duContext->topContext());
      if(!t)
        return IndexedType();
    }
  }
  
  return t->indexed();
}

CodeCompletionContext::~CodeCompletionContext() {
}

bool CodeCompletionContext::isValidPosition() {
  if( m_text.isEmpty() )
    return true;
  //If we are in a string or comment, we should not complete anything
  QString markedText = clearComments(m_text, '$');
  markedText = clearStrings(markedText,'$');

  if( markedText[markedText.length()-1] == '$' ) {
    //We are within a comment or string
    kDebug(9007) << "code-completion position is invalid, marked text: \n\"" << markedText << "\"\n unmarked text:\n" << m_text << "\n";
    return false;
  }
  return true;
}

QString originalOperator( const QString& str ) {
  if( str == "[" )
    return "[]";
  return str;
}

QString CodeCompletionContext::getEndOperator( const QString& str ) const {

  for( QStringList::const_iterator it = allOperators.begin(); it != allOperators.end(); ++it )
    if( str.endsWith(*it) )
      return *it;
  return QString();
}

QString CodeCompletionContext::getEndFunctionOperator( const QString& str ) const {
  return originalOperator( getEndOperator( str ) );
}

bool CodeCompletionContext::endsWithOperator( const QString& str ) const {
  return !getEndOperator(str).isEmpty();
}

// QList<KDevelop::AbstractType::Ptr> CodeCompletionContext::additionalMatchTypes() const {
//   QList<KDevelop::AbstractType::Ptr> ret;
//   if( m_operator == "=" && m_expressionResult.isValid() && m_expressionResult.isInstance ) {
//     //Conversion to the left operand-type
//     ret << m_expressionResult.type.type();
//   }
//   return ret;
// }

void CodeCompletionContext::preprocessText( int line ) {

  LOCKDUCHAIN;

  QSet<IndexedString> disableMacros;
  disableMacros.insert(IndexedString("SIGNAL"));
  disableMacros.insert(IndexedString("SLOT"));
  disableMacros.insert(IndexedString("emit"));
  
  if( m_duContext ) {
  m_text = preprocess( m_text,  dynamic_cast<Cpp::EnvironmentFile*>(m_duContext->topContext()->parsingEnvironmentFile().data()), line, disableMacros );
  }else{
    kWarning() << "error: no ducontext";
  }
}

CodeCompletionContext::MemberAccessOperation CodeCompletionContext::memberAccessOperation() const {
  return m_memberAccessOperation;
}

CodeCompletionContext* CodeCompletionContext::parentContext() {
  return static_cast<CodeCompletionContext*>(KDevelop::CodeCompletionContext::parentContext());
}

void getOverridable(DUContext* base, DUContext* current, QMap< QPair<IndexedType, IndexedString>, KDevelop::CompletionTreeItemPointer >& overridable, CodeCompletionContext::Ptr completionContext) {
  foreach(Declaration* decl, current->localDeclarations()) {
    ClassFunctionDeclaration* classFun = dynamic_cast<ClassFunctionDeclaration*>(decl);
    if(classFun && classFun->isVirtual() && !classFun->isConstructor() && !classFun->isDestructor()) {
      QPair<IndexedType, IndexedString> key = qMakePair(classFun->indexedType(), classFun->identifier().identifier());
      if(!overridable.contains(key) && base->findLocalDeclarations(classFun->identifier(), SimpleCursor::invalid(), 0, key.first.type()).isEmpty())
        overridable.insert(key, KDevelop::CompletionTreeItemPointer(new ImplementationHelperItem(ImplementationHelperItem::Override, DeclarationPointer(decl), completionContext)));
    }
  }

  foreach(const DUContext::Import &import, current->importedParentContexts())
    getOverridable(base, import.context(base->topContext()), overridable, completionContext);
}

// #ifndef TEST_COMPLETION

QList< KSharedPtr< KDevelop::CompletionTreeElement > > CodeCompletionContext::ungroupedElements() {
  return m_storedUngroupedItems;
}

QList<CompletionTreeItemPointer> CodeCompletionContext::completionItems(const KDevelop::SimpleCursor& position, bool& shouldAbort, bool fullCompletion) {
    LOCKDUCHAIN;
    QList<CompletionTreeItemPointer> items;

    if(!m_duContext || !m_valid)
      return items;

    typedef QPair<Declaration*, int> DeclarationDepthPair;

    bool ignoreParentContext = false;

    if(!m_storedItems.isEmpty()) {
      items = m_storedItems;
    }else{
      switch(memberAccessOperation()) {
        case MemberAccess:
        case ArrowMemberAccess:
        case StaticMemberChoose:
        case MemberChoose:
          if( memberAccessContainer().isValid() ||memberAccessOperation() == Cpp::CodeCompletionContext::StaticMemberChoose )
          {
            bool typeIsConst = false;
            AbstractType::Ptr expressionTarget = TypeUtils::targetType(m_expressionResult.type.type(), m_duContext->topContext());
            if (expressionTarget && (expressionTarget->modifiers() & AbstractType::ConstModifier))
              typeIsConst = true;
            
            QSet<QualifiedIdentifier> hadNamespaceDeclarations; //Used to show only one namespace-declaration per namespace
            QList<DUContext*> containers = memberAccessContainers();
            ifDebug( kDebug() << "got" << containers.size() << "member-access containers"; )
            if( !containers.isEmpty() ) {
              QSet<DUContext*> had;
              foreach(DUContext* ctx, containers) {
                if(had.contains(ctx)) //We need this so we don't process the same container twice
                  continue;
                had.insert(ctx);

                if (shouldAbort)
                  return items;

                foreach( const DeclarationDepthPair& decl, Cpp::hideOverloadedDeclarations( ctx->allDeclarations(ctx->range().end, m_duContext->topContext(), false ) ) ) {
                  //If we have StaticMemberChoose, which means A::Bla, show only static members, except if we're within a class that derives from the container
                  ClassMemberDeclaration* classMember = dynamic_cast<ClassMemberDeclaration*>(decl.first);

                  if(classMember && !filterDeclaration(classMember, ctx, typeIsConst))
                    continue;
                  else if(!filterDeclaration(decl.first, ctx))
                    continue;
                  
                  if(decl.first->kind() == Declaration::Namespace) {
                    QualifiedIdentifier id = decl.first->qualifiedIdentifier();
                    if(hadNamespaceDeclarations.contains(id))
                      continue;
                    
                    hadNamespaceDeclarations.insert(id);
                  }
                    
                  if(memberAccessOperation() != Cpp::CodeCompletionContext::StaticMemberChoose) {
                    if(decl.first->kind() != Declaration::Instance)
                      continue;
                    if(classMember && classMember->isStatic())
                      continue; //Skip static class members when not doing static access
                    if(decl.first->abstractType().cast<EnumeratorType>())
                      continue; //Skip enumerators
                  }else{
                    ///@todo what NOT to show on static member choose? Actually we cannot hide all non-static functions, because of function-pointers
                  }

                  if(!decl.first->identifier().isEmpty())
                    items << CompletionTreeItemPointer( new NormalDeclarationCompletionItem( DeclarationPointer(decl.first), CodeCompletionContext::Ptr(this), decl.second ) );
                }
              }
            } else {
              kDebug() << "missing-include completion for" << m_expression << m_expressionResult.toString();
                eventuallyAddGroup(i18n("Not Included Container"), 700, missingIncludeCompletionItems(m_expression, QString(), m_expressionResult, m_duContext.data(), 0, true ));
            }
          }
          break;
        case ReturnAccess:
          {
            DUContext* functionContext = m_duContext.data();
            while(functionContext && !functionContext->owner())
              functionContext = functionContext->parentContext();
            if(functionContext && functionContext->owner()) {
              FunctionType::Ptr funType = functionContext->owner()->type<FunctionType>();
              if(funType) {
                if(funType->returnType()) {
                  items << CompletionTreeItemPointer( new TypeConversionCompletionItem( "return " + funType->returnType()->toString(), funType->returnType()->indexed(), depth(), KSharedPtr <Cpp::CodeCompletionContext >(this) ) );
                }
              }
            }
          }
        break;
        case TemplateAccess:
          {
            AbstractType::Ptr type = m_expressionResult.type.type();
            IdentifiedType* identified = dynamic_cast<IdentifiedType*>(type.unsafeData());
            Declaration* decl = 0;
            if(identified)
              decl = identified->declaration( m_duContext->topContext());
            if(!decl && !m_expressionResult.allDeclarations.isEmpty())
              decl = m_expressionResult.allDeclarations[0].getDeclaration(m_duContext->topContext());
            if(decl) {
              NormalDeclarationCompletionItem* item = new NormalDeclarationCompletionItem( KDevelop::DeclarationPointer(decl),  KSharedPtr <Cpp::CodeCompletionContext >(this), 0, 0 );
              item->m_isTemplateCompletion = true;
              items << CompletionTreeItemPointer( item );
            }else{
              items += missingIncludeCompletionItems(m_expression, QString(), m_expressionResult, m_duContext.data(), depth(), true );
            }
          }
          break;
        case FunctionCallAccess:
          {
            kDebug() << "functionCallAccess" << functions().count() << m_expression;
            //Don't show annoying empty argument-hints
/*            if(parentContext->m_contextType != BinaryOperatorFunctionCall && parentContext->functions().size() == 0)
              break;*/
            //When there is too many overloaded functions, do not show them. They can just be too many.
            if (functions().count() > maxOverloadedOperatorArgumentHints) {
              items << CompletionTreeItemPointer( new NormalDeclarationCompletionItem( KDevelop::DeclarationPointer(),  KSharedPtr <Cpp::CodeCompletionContext >(this), 0, 0 ) );
              if(functions().count())
                items.back()->asItem<NormalDeclarationCompletionItem>()->alternativeText = i18n("%1 overloads of", functions().count()) + " " + functionName();
            }else if(functions().count() == 0 && additionalContextType() != Cpp::CodeCompletionContext::BinaryOperatorFunctionCall) {
              items += missingIncludeCompletionItems(m_expression, QString(), m_expressionResult, m_duContext.data(), depth(), true );
            }else if(!functions().isEmpty()) {
              int num = 0;
              foreach( const Cpp::CodeCompletionContext::Function &function, functions() ) {
                items << CompletionTreeItemPointer( new NormalDeclarationCompletionItem( function.function.declaration(), KSharedPtr <Cpp::CodeCompletionContext >(this), 0, num ) );
                ++num;
              }
            }

            if(additionalContextType() == Cpp::CodeCompletionContext::BinaryOperatorFunctionCall) {
              //Argument-hints for builtin operators
              AbstractType::Ptr type = m_expressionResult.type.type();
              if(m_expressionResult.isValid() && m_expressionResult.isInstance && type) {
                IntegralType::Ptr integral = type.cast<IntegralType>();

                if(!integral && (arithmeticComparisonOperators.contains(m_operator) || binaryArithmeticOperators.contains(m_operator))) {
                  ///There is one more chance: If the type can be converted to an integral type, C++ will convert it first, and then
                  ///apply its builtin operators
                  integral = IntegralType::Ptr(new IntegralType(KDevelop::IntegralType::TypeInt));
                  TypeConversion conv(m_duContext->topContext());
                  if(!conv.implicitConversion(m_expressionResult.type, integral->indexed()))
                    integral = IntegralType::Ptr(); //No conversion possible
                }
                
                if( m_operator == "[]" && (type.cast<KDevelop::ArrayType>() || type.cast<KDevelop::PointerType>())) {
                  IntegralType::Ptr t(new IntegralType(IntegralType::TypeInt));
                  t->setModifiers(IntegralType::UnsignedModifier);
                  QString showName = "operator []";
                  items << CompletionTreeItemPointer( new TypeConversionCompletionItem( showName, t->indexed(), depth(), KSharedPtr <Cpp::CodeCompletionContext >(this) ) );
                }

                if( m_operator == "=" || integral ) {
                  ///Conversion to the left operand-type, builtin operators on integral types
                  IndexedType useType = integral ? integral->indexed() : m_expressionResult.type;
                  QString showName = functionName();
                  if(useType.type())
                    showName = useType.type()->toString() + " " + m_operator;

                  if(useType == m_expressionResult.type && m_expressionResult.allDeclarations.size() == 1) {
                    Declaration* decl = m_expressionResult.allDeclarations[0].getDeclaration(m_duContext->topContext());
                    if(decl)
                      showName = decl->toString() + " " + m_operator;
                  }

                  items << CompletionTreeItemPointer( new TypeConversionCompletionItem( showName, useType, depth(), KSharedPtr <Cpp::CodeCompletionContext >(this) ) );
                }
              }

//                 items.back()->asItem<NormalDeclarationCompletionItem>()->alternativeText = functionName();
            }
          }
          break;
        case IncludeListAccess:
          //m_storedItems is used for include-list access
          {
            //Include-file completion
            int cnt = 0;
            QList<Cpp::IncludeItem> allIncludeItems = includeItems();
            foreach(const Cpp::IncludeItem& includeItem, allIncludeItems) {
              if (shouldAbort)
                return items;

              items << CompletionTreeItemPointer( new IncludeFileCompletionItem(includeItem) );
              ++cnt;
            }
            kDebug(9007) << "Added " << cnt << " include-files to completion-list";
          }
          break;
        case SignalAccess:
        case SlotAccess:
        {
        KDevelop::IndexedDeclaration connectedSignal;
        if(!m_connectedSignalIdentifier.isEmpty()) {
          ///Create an additional argument-hint context that shows information about the signal we connect to
          if(parentContext() && parentContext()->m_knownArgumentTypes.count() > 1 && parentContext()->m_knownArgumentTypes[0].type.isValid()) {
            StructureType::Ptr signalContainerType = TypeUtils::targetType(parentContext()->m_knownArgumentTypes[0].type.type(), m_duContext->topContext()).cast<StructureType>();
           if(signalContainerType) {
//             kDebug() << "searching signal in container" << signalContainerType->toString() << m_connectedSignalIdentifier.toString();
               Declaration* signalContainer = signalContainerType->declaration(m_duContext->topContext());
              if(signalContainer && signalContainer->internalContext()) {
                IndexedString signature(m_connectedSignalNormalizedSignature);
                foreach(const DeclarationDepthPair &decl, signalContainer->internalContext()->allDeclarations( SimpleCursor::invalid(), m_duContext->topContext(), false )) {
                  if(decl.first->identifier() == m_connectedSignalIdentifier) {
                    if(QtFunctionDeclaration* classFun = dynamic_cast<QtFunctionDeclaration*>(decl.first)) {
                      if(classFun->isSignal() && classFun->normalizedSignature() == signature) {
                        //Match
                        NormalDeclarationCompletionItem* item = new NormalDeclarationCompletionItem( DeclarationPointer(decl.first), CodeCompletionContext::Ptr(parentContext()), decl.second + 50);
                        item->useAlternativeText = true;
                        m_connectedSignal = IndexedDeclaration(decl.first);
                        item->alternativeText = i18n("Connect to") + " " + decl.first->qualifiedIdentifier().toString() + "(" + QString::fromUtf8(m_connectedSignalNormalizedSignature) + ")";
                        item->m_isQtSignalSlotCompletion = true;
                        items << CompletionTreeItemPointer(item);
                        connectedSignal = IndexedDeclaration(decl.first);
                      }
                    }
                  }
                }
              }
            }
          }
        }
        if( memberAccessContainer().isValid() ) {
          QList<CompletionTreeItemPointer> signalSlots;
          ///Collect all slots/signals to show
          AbstractType::Ptr type = memberAccessContainer().type.type();
          IdentifiedType* identified = dynamic_cast<IdentifiedType*>(type.unsafeData());
          if(identified) {
            Declaration* decl = identified->declaration(m_duContext->topContext());
            if(decl && decl->internalContext() /*&& Cpp::findLocalDeclarations(decl->internalContext(), Identifier("QObject"), m_duContext->topContext()).count()*/) { //hacky test whether it's a QObject
              ///@todo Always allow this when the class is within one of the open projects. Problem: The project lookup is not threadsafe
              if(connectedSignal.isValid() && m_localClass.data() == decl->internalContext()) { ///Create implementation-helper to add a slot
                signalSlots << CompletionTreeItemPointer(new ImplementationHelperItem(ImplementationHelperItem::CreateSignalSlot, DeclarationPointer(connectedSignal.data()), CodeCompletionContext::Ptr(this)));
              }
              
              foreach(const DeclarationDepthPair &candidate, decl->internalContext()->allDeclarations(SimpleCursor::invalid(), m_duContext->topContext(), false) ) {
                if(QtFunctionDeclaration* classFun = dynamic_cast<QtFunctionDeclaration*>(candidate.first)) {
                  if((classFun->isSignal() && !m_onlyShowSlots) || (memberAccessOperation() == SlotAccess && classFun->isSlot() && filterDeclaration(classFun))) {
                    NormalDeclarationCompletionItem* item = new NormalDeclarationCompletionItem( DeclarationPointer(candidate.first), CodeCompletionContext::Ptr(this), candidate.second );
                    item->m_isQtSignalSlotCompletion = true;
                    if(!m_connectedSignalIdentifier.isEmpty()) {
                      item->m_fixedMatchQuality = 0;
                      //Compute a match-quality, by comparing the strings
                      QByteArray thisSignature = classFun->normalizedSignature().byteArray();
                      if(m_connectedSignalNormalizedSignature.startsWith(thisSignature) || (m_connectedSignalNormalizedSignature.isEmpty() && thisSignature.isEmpty())) {
                        QByteArray remaining = m_connectedSignalNormalizedSignature.mid(thisSignature.length());
                        int remainingElements = remaining.split(',').count();
                        if(remaining.isEmpty())
                          item->m_fixedMatchQuality = 10;
                        else if(remainingElements < 4)
                          item->m_fixedMatchQuality  = 6 - remainingElements;
                        else
                          item->m_fixedMatchQuality = 2;
                      }
                    }else{
                      item->m_fixedMatchQuality = 10;
                    }
                    signalSlots << CompletionTreeItemPointer( item );
                  }
                }
              }
              
              eventuallyAddGroup(i18n("Signals/Slots"), 10, signalSlots);
            }
          }
        }
        }
        //Since there is 2 connect() functions, the third argument may be a slot as well as a QObject*, so also
        //give normal completion items
       if(parentContext() && parentContext()->m_knownArgumentExpressions.size() != 2)
          break;
        default:
          if(depth() == 0)
            standardAccessCompletionItems(position, items);
          break;
      }
    }

    if(!ignoreParentContext && fullCompletion && m_parentContext && (!noMultipleBinaryOperators || m_contextType != BinaryOperatorFunctionCall || parentContext()->m_contextType != BinaryOperatorFunctionCall))
      items = parentContext()->completionItems( position, shouldAbort, fullCompletion ) + items;

    if(depth() == 0) {
      //Eventually add missing include-completion in cases like SomeNamespace::NotIncludedClass|
      if(memberAccessOperation() == StaticMemberChoose) {
#ifndef TEST_COMPLETION  
        MissingIncludeCompletionModel::self().startWithExpression(m_duContext, m_expression + "::", m_followingText.trimmed());
#endif
      }

      if(m_duContext->type() == DUContext::Class && !parentContext()) {
        //Show override helper items
        QMap< QPair<IndexedType, IndexedString>, KDevelop::CompletionTreeItemPointer > overridable;
        foreach(const DUContext::Import &import, m_duContext->importedParentContexts())
          getOverridable(m_duContext.data(), import.context(m_duContext->topContext()), overridable, Ptr(this));
        
        if(!overridable.isEmpty()) {
          eventuallyAddGroup(i18n("Virtual Override"), 0, overridable.values());
        }
      }

      if(!parentContext() && (m_duContext->type() == DUContext::Namespace || m_duContext->type() == DUContext::Global)) {
        QList<CompletionTreeItemPointer> helpers = getImplementationHelpers();
        if(!helpers.isEmpty()) {
          eventuallyAddGroup(i18n("Implement Function"), 0, helpers);
        }
      }
    }

    return items;
}

QList<CompletionTreeItemPointer> CodeCompletionContext::getImplementationHelpers() {
  QList<CompletionTreeItemPointer> ret;
#ifndef TEST_COMPLETION
  TopDUContext* searchInContext = m_duContext->topContext();
  
  if(searchInContext)
    ret += getImplementationHelpersInternal(m_duContext->scopeIdentifier(true), searchInContext);
  
  if(!CppLanguageSupport::self()->isHeader( searchInContext->url().toUrl() )) {
    KUrl headerUrl = CppLanguageSupport::self()->sourceOrHeaderCandidate( searchInContext->url().toUrl(), true );
    searchInContext = CppLanguageSupport::self()->standardContext(headerUrl);
  }

  if(searchInContext)
    ret += getImplementationHelpersInternal(m_duContext->scopeIdentifier(true), searchInContext);
 
#endif
  return ret;
}

QList<CompletionTreeItemPointer> CodeCompletionContext::getImplementationHelpersInternal(QualifiedIdentifier minimumScope, DUContext* context) {
  QList<CompletionTreeItemPointer> ret;

  foreach(Declaration* decl, context->localDeclarations()) {
    ClassFunctionDeclaration* classFun = dynamic_cast<ClassFunctionDeclaration*>(decl);
    AbstractFunctionDeclaration* funDecl = dynamic_cast<AbstractFunctionDeclaration*>(decl);
    if(funDecl && (!classFun || !classFun->isAbstract()) && !decl->isDefinition() && !FunctionDefinition::definition(decl) && decl->qualifiedIdentifier().toString().startsWith(minimumScope.toString()))
      ret << KDevelop::CompletionTreeItemPointer(new ImplementationHelperItem(ImplementationHelperItem::CreateDefinition, DeclarationPointer(decl), KSharedPtr<CodeCompletionContext>(this)));
  }

  foreach(DUContext* child, context->childContexts())
    if(child->type() == DUContext::Namespace || child->type() == DUContext::Class)
      ret += getImplementationHelpersInternal(minimumScope, child);
  return ret;
}

QualifiedIdentifier CodeCompletionContext::requiredPrefix(Declaration* decl) const {
  QualifiedIdentifier worstCase = decl->context()->scopeIdentifier(true);
  if(!m_duContext)
    return worstCase;
  QualifiedIdentifier currentPrefix;

  while(1) {
    QList<Declaration*> found = m_duContext->findDeclarations( currentPrefix + decl->identifier() );
    if(found.contains(decl))
      return currentPrefix;

    if(currentPrefix.count() >= worstCase.count()) {
      return worstCase;
    }else {
      currentPrefix.push(worstCase.at(currentPrefix.count()));
    }
  }
}

QList< KSharedPtr< KDevelop::CompletionTreeItem > > CodeCompletionContext::specialItemsForArgumentType(TypePtr< KDevelop::AbstractType > type) {
  QList< KSharedPtr< KDevelop::CompletionTreeItem > > items;
  if(EnumerationType::Ptr enumeration = TypeUtils::realType(type, m_duContext->topContext()).cast<EnumerationType>()) {
    Declaration* enumDecl = enumeration->declaration(m_duContext->topContext());
    if(enumDecl && enumDecl->internalContext()) {

      QualifiedIdentifier prefix = requiredPrefix(enumDecl);

      DUContext* enumInternal = enumDecl->internalContext();
      foreach(Declaration* enumerator, enumInternal->localDeclarations()) {
        QualifiedIdentifier id = prefix + enumerator->identifier();
        items << CompletionTreeItemPointer(new NormalDeclarationCompletionItem( DeclarationPointer(enumerator), Ptr(this), 0 ));
        static_cast<NormalDeclarationCompletionItem*>(items.back().data())->alternativeText = id.toString();
        static_cast<NormalDeclarationCompletionItem*>(items.back().data())->useAlternativeText = true;
      }
    }
  }
  return items;
}

void CodeCompletionContext::standardAccessCompletionItems(const KDevelop::SimpleCursor& position, QList<CompletionTreeItemPointer>& items) {
  //Normal case: Show all visible declarations
  typedef QPair<Declaration*, int> DeclarationDepthPair;
  QSet<QualifiedIdentifier> hadNamespaceDeclarations;

  bool typeIsConst = false;
  if (Declaration* func = Cpp::localFunctionFromCodeContext(m_duContext.data())) {
    if (func->abstractType() && (func->abstractType()->modifiers() & AbstractType::ConstModifier))
      typeIsConst = true;
  }

  QList<DeclarationDepthPair> decls = m_duContext->allDeclarations(m_duContext->type() == DUContext::Class ? m_duContext->range().end : position, m_duContext->topContext());

  //Collect the contents of unnamed namespaces
  QList<DUContext*> unnamed = m_duContext->findContexts(DUContext::Namespace, QualifiedIdentifier(), position);
  foreach(DUContext* ns, unnamed)
    decls += ns->allDeclarations(position, m_duContext->topContext(), false);

  if(m_duContext) {
    //Collect the Declarations from all "using namespace" imported contexts
    QList<Declaration*> imports = m_duContext->findDeclarations( globalImportIdentifier, position );
    QSet<QualifiedIdentifier> ids;
    foreach(Declaration* importDecl, imports) {
      NamespaceAliasDeclaration* aliasDecl = dynamic_cast<NamespaceAliasDeclaration*>(importDecl);
      if(aliasDecl) {
        ids.insert(aliasDecl->importIdentifier());
      }else{
        kDebug() << "Import is not based on NamespaceAliasDeclaration";
      }
    }
    
    QualifiedIdentifier ownNamespaceScope = Cpp::namespaceScopeComponentFromContext(m_duContext->scopeIdentifier(true), m_duContext.data(), m_duContext->topContext());
    if(!ownNamespaceScope.isEmpty())
      ids += ownNamespaceScope;

    foreach(const QualifiedIdentifier &id, ids) {
      QList<DUContext*> importedContexts = m_duContext->findContexts( DUContext::Namespace, id );
      foreach(DUContext* context, importedContexts) {
        if(context->range().contains(m_duContext->range()))
          continue; //If the context surrounds the current one, the declarations are visible through allDeclarations(..).
        foreach(Declaration* decl, context->localDeclarations())
          if(filterDeclaration(decl))
            decls << qMakePair(decl, 1200);
      }
    }
  }

  QList<DeclarationDepthPair> oldDecls = decls;
  decls.clear();
  
  //Remove pure function-definitions before doing overload-resolution, so they don't hide their own declarations.
  foreach( const DeclarationDepthPair& decl, oldDecls )
    if(!dynamic_cast<FunctionDefinition*>(decl.first) || !static_cast<FunctionDefinition*>(decl.first)->hasDeclaration()) {
      if(decl.first->kind() == Declaration::Namespace) {
        QualifiedIdentifier id = decl.first->qualifiedIdentifier();
        if(hadNamespaceDeclarations.contains(id))
          continue;
        
        hadNamespaceDeclarations.insert(id);
      }
      
      if(filterDeclaration(decl.first, 0, true, typeIsConst)) {
        decls << decl;
      }
    }
    
  decls = Cpp::hideOverloadedDeclarations(decls);

  foreach( const DeclarationDepthPair& decl, decls )
    items << CompletionTreeItemPointer( new NormalDeclarationCompletionItem(DeclarationPointer(decl.first), Ptr(this), decl.second ) );

  ///Eventually show additional specificly known items for the matched argument-type, like for example enumerators for enum types
  CodeCompletionContext* parent = parentContext();
  if(parent) {
    if(parent->memberAccessOperation() == FunctionCallAccess) {
      foreach(const Cpp::OverloadResolutionFunction& function, parent->functions()) {
        if(function.function.isValid() && function.function.isViable() && function.function.declaration()) {
          //uint parameterNumber = parent->m_knownArgumentExpressions.size() + function.matchedArguments;
          Declaration* functionDecl = function.function.declaration().data();
          if(functionDecl->type<FunctionType>()->arguments().count() > function.matchedArguments) {
            items += specialItemsForArgumentType(functionDecl->type<FunctionType>()->arguments()[function.matchedArguments]);
          }
        }
      }
    }
  }

  ///Eventually add a "this" item
  DUContext* functionContext = m_duContext.data();
  if(!m_onlyShowSignals && !m_onlyShowSlots && !m_onlyShowTypes) {
    while(functionContext && functionContext->type() == DUContext::Other && functionContext->parentContext()->type() == DUContext::Other)
      functionContext = functionContext->parentContext();
  }

  ClassFunctionDeclaration* classFun = dynamic_cast<ClassFunctionDeclaration*>(DUChainUtils::declarationForDefinition(functionContext->owner(), m_duContext->topContext()));
  
  if(classFun && !classFun->isStatic() && classFun->context()->owner() && !m_onlyShowSignals && !m_onlyShowSlots && !m_onlyShowTypes) {
    AbstractType::Ptr classType = classFun->context()->owner()->abstractType();
    if(classFun->abstractType()->modifiers() & AbstractType::ConstModifier)
      classType->setModifiers((AbstractType::CommonModifiers)(classType->modifiers() | AbstractType::ConstModifier));
    PointerType::Ptr thisPointer(new PointerType());
    thisPointer->setModifiers(AbstractType::ConstModifier);
    thisPointer->setBaseType(classType);
    KSharedPtr<TypeConversionCompletionItem> item( new TypeConversionCompletionItem("this", thisPointer->indexed(), 0, KSharedPtr <Cpp::CodeCompletionContext >(this)) );
    item->setPrefix(thisPointer->toString());
    QList<CompletionTreeItemPointer> lst;
    lst += CompletionTreeItemPointer(item.data());
    eventuallyAddGroup(i18n("C++ Builtin"), 800, lst);
  }

  //Eventually add missing include-completion in cases like NotIncludedClass|
//   if(!m_followingText.trimmed().isEmpty()) {
//     uint oldItemCount = items.count();
//     items += missingIncludeCompletionItems(totalExpression, m_followingText.trimmed() + ": ", ExpressionEvaluationResult(), m_duContext.data(), 0);
#ifndef TEST_COMPLETION
    MissingIncludeCompletionModel::self().startWithExpression(m_duContext, QString(), m_followingText.trimmed());
#endif
//     kDebug() << QString("added %1 missing-includes for %2").arg(items.count()-oldItemCount).arg(totalExpression);
//   }
  
  eventuallyAddGroup(i18n("C++ Builtin"), 800, keywordCompletionItems());
}

bool CodeCompletionContext::visibleFromWithin(KDevelop::Declaration* decl, DUContext* currentContext) {
  if(!decl || !currentContext)
    return false;
  if(currentContext->imports(decl->context()))
    return true;
  
  return visibleFromWithin(decl, currentContext->parentContext());
}

bool  CodeCompletionContext::filterDeclaration(Declaration* decl, DUContext* declarationContext, bool dynamic, bool typeIsConst) {
  if(!decl)
    return true;

  if(dynamic_cast<TemplateParameterDeclaration*>(decl) && !visibleFromWithin(decl, m_duContext.data()))
    return false;
  
  static IndexedIdentifier friendIdentifier(Identifier("friend"));
  
  if(decl->indexedIdentifier() == friendIdentifier)
    return false;
  
  if(m_onlyShowTypes && decl->kind() != Declaration::Type && decl->kind() != Declaration::Namespace)
    return false;
    
  if(m_onlyShowSignals || m_onlyShowSlots) {
    Cpp::QtFunctionDeclaration* qtFunction = dynamic_cast<Cpp::QtFunctionDeclaration*>(decl);
    if(!qtFunction || (m_onlyShowSignals && !qtFunction->isSignal()) || (m_onlyShowSlots && !qtFunction->isSlot()))
      return false;
  }
  
  if(dynamic && decl->context()->type() == DUContext::Class) {
    ClassMemberDeclaration* classMember = dynamic_cast<ClassMemberDeclaration*>(decl);
    if(classMember)
      return filterDeclaration(classMember, declarationContext, typeIsConst);
  }
  
  return true;
}

bool  CodeCompletionContext::filterDeclaration(ClassMemberDeclaration* decl, DUContext* declarationContext, bool typeIsConst) {
  if(doAccessFiltering && decl) {
    if (typeIsConst && decl->type<FunctionType>() && !(decl->abstractType()->modifiers() & AbstractType::ConstModifier))
      return false;
    if(!Cpp::isAccessible(m_localClass ? m_localClass.data() : m_duContext.data(), decl, m_duContext->topContext(), declarationContext))
      return false;
  }
  return filterDeclaration((Declaration*)decl, declarationContext, false);
}

void CodeCompletionContext::replaceCurrentAccess(QString old, QString _new)
{
  IDocument* document = ICore::self()->documentController()->documentForUrl(m_duContext->url().toUrl());
  if(document) {
    KTextEditor::Document* textDocument = document->textDocument();
    if(textDocument) {
      KTextEditor::View* activeView = textDocument->activeView();
      if(activeView) {
        KTextEditor::Cursor cursor = activeView->cursorPosition();
        KTextEditor::Range oldRange = KTextEditor::Range(cursor-KTextEditor::Cursor(0,old.length()), cursor);
        if(oldRange.start().column() >= 0 && textDocument->text(oldRange) == old) {
          textDocument->replaceText(oldRange, _new);
        }
      }
    }
  }
}

int CodeCompletionContext::matchPosition() const {
  return m_knownArgumentExpressions.count();
}

void CodeCompletionContext::eventuallyAddGroup(QString name, int priority, QList< KSharedPtr< KDevelop::CompletionTreeItem > > items) {
  if(items.isEmpty())
    return;
  KDevelop::CompletionCustomGroupNode* node = new KDevelop::CompletionCustomGroupNode(name, priority);
  node->appendChildren(items);
  m_storedUngroupedItems << CompletionTreeElementPointer(node);
}

QList< KSharedPtr< KDevelop::CompletionTreeItem > > CodeCompletionContext::keywordCompletionItems() {
  QList<CompletionTreeItemPointer> ret;
  #ifdef TEST_COMPLETION
  return ret;
  #endif
  #define ADD_TYPED_TOKEN_S(X, type) ret << CompletionTreeItemPointer( new TypeConversionCompletionItem(X, type, 0, KSharedPtr<Cpp::CodeCompletionContext>(this)) )
  #define ADD_TYPED_TOKEN(X, type) ADD_TYPED_TOKEN_S(#X, type)
  
  #define ADD_TOKEN(X) ADD_TYPED_TOKEN(X, KDevelop::IndexedType())
  #define ADD_TOKEN_S(X) ADD_TYPED_TOKEN_S(X, KDevelop::IndexedType())

  bool restrictedItems = m_onlyShowSignals || m_onlyShowSlots || m_onlyShowTypes;
  
  if(!restrictedItems || m_onlyShowTypes) {
    ADD_TOKEN(bool);
    ADD_TOKEN(char);
    ADD_TOKEN(const);
    ADD_TOKEN(double);
    ADD_TOKEN(enum);
    ADD_TOKEN(float);
    ADD_TOKEN(int);
    ADD_TOKEN(long);
    ADD_TOKEN(mutable);
    ADD_TOKEN(register);
    ADD_TOKEN(short);
    ADD_TOKEN(signed);
    ADD_TOKEN(struct);
    ADD_TOKEN(template);
    ADD_TOKEN(typename);
    ADD_TOKEN(union);
    ADD_TOKEN(unsigned);
    ADD_TOKEN(void);
    ADD_TOKEN(volatile);
    ADD_TOKEN(wchar_t);
  }
  
  if(restrictedItems && (m_duContext->type() == DUContext::Other || m_duContext->type() == DUContext::Function))
    return ret;
  
  if(m_duContext->type() == DUContext::Class) {
    ADD_TOKEN_S("Q_OBJECT");
    ADD_TOKEN(private);
    ADD_TOKEN(protected);
    ADD_TOKEN(public);
    ADD_TOKEN_S("signals");
    ADD_TOKEN_S("slots");
    ADD_TOKEN(virtual);
    ADD_TOKEN(friend);
    ADD_TOKEN(explicit);
  }
  
  if(m_duContext->type() == DUContext::Other) {
    ADD_TOKEN(break);
    ADD_TOKEN(case);
    ADD_TOKEN(and);
    ADD_TOKEN(and_eq);
    ADD_TOKEN(asm);
    ADD_TOKEN(bitand);
    ADD_TOKEN(bitor);
    ADD_TOKEN(catch);
    ADD_TOKEN(const_cast);
    ADD_TOKEN(default);
    ADD_TOKEN(delete);
    ADD_TOKEN(do);
    ADD_TOKEN(dynamic_cast);
    ADD_TOKEN(else);
    ADD_TOKEN_S("emit");
    ADD_TOKEN(for);
    ADD_TOKEN(goto);
    ADD_TOKEN(if);
    ADD_TOKEN(incr);
    ADD_TOKEN(new);
    ADD_TOKEN(not);
    ADD_TOKEN(not_eq);
    ADD_TOKEN(or);
    ADD_TOKEN(or_eq);
    ADD_TOKEN(reinterpret_cast);
    ADD_TOKEN(return);
    ADD_TOKEN(static_cast);
    ADD_TOKEN(switch);
    ADD_TOKEN(try);
    ADD_TOKEN(typeid);
    ADD_TOKEN(while);
    ADD_TOKEN(xor);
    ADD_TOKEN(xor_eq);
    ADD_TOKEN(continue);
  }else{
    ADD_TOKEN(inline);
  }
  
  if(m_duContext->type() == DUContext::Global) {
    ADD_TOKEN(export);
    ADD_TOKEN(extern);
    ADD_TOKEN(namespace);
  }
  
  ADD_TOKEN(auto);
  ADD_TOKEN(class);
  ADD_TOKEN(operator);
  ADD_TOKEN(sizeof);
  ADD_TOKEN(static);
  ADD_TOKEN(throw);
  ADD_TOKEN(typedef);
  ADD_TOKEN(using);

  ConstantIntegralType::Ptr trueType(new ConstantIntegralType(IntegralType::TypeBoolean));
  trueType->setValue<bool>(true);
  
  ADD_TYPED_TOKEN(true, trueType->indexed());

  ConstantIntegralType::Ptr falseType(new ConstantIntegralType(IntegralType::TypeBoolean));
  falseType->setValue<bool>(false);

  ADD_TYPED_TOKEN(false, falseType->indexed());
  
  return ret;
}

QString CodeCompletionContext::followingText() const {
  return m_followingText;
}

void CodeCompletionContext::setFollowingText(QString str) {
  m_followingText = str;
}


}