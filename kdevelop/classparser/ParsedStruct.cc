/********************************************************************
* Name    : Implementation of a parsed structure.                   *
* ------------------------------------------------------------------*
* File    : ParsedStruct.cc                                         *
* Author  : Jonas Nordin(jonas.nordin@cenacle.se)                   *
* Date    : Tue Mar 30 11:09:36 CEST 1999                           *
*                                                                   *
* ------------------------------------------------------------------*
* Purpose :                                                         *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
* Usage   :                                                         *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
* Macros:                                                           *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
* Types:                                                            *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
* Functions:                                                        *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
* Modifications:                                                    *
*                                                                   *
*                                                                   *
*                                                                   *
* ------------------------------------------------------------------*
*********************************************************************/

#include "ParsedStruct.h"
#include "ParsedItem.h"
#include "ParsedClassItem.h"
#include <iostream.h>
#include <assert.h>

/*********************************************************************
 *                                                                   *
 *                     CREATION RELATED METHODS                      *
 *                                                                   *
 ********************************************************************/

/*-------------------------------------- CParsedStruct::CParsedStruct()
 * CParsedStruct()
 *   Constructor.
 *
 * Parameters:
 *   -
 * Returns:
 *   -
 *-----------------------------------------------------------------*/
CParsedStruct::CParsedStruct()
  : memberIterator( members )
{
  setItemType( PIT_STRUCT );
}

/*------------------------------------- CClassParser::~CClassParser()
 * ~CClassParser()
 *   Destructor.
 *
 * Parameters:
 *   -
 * Returns:
 *   -
 *-----------------------------------------------------------------*/
CParsedStruct::~CParsedStruct()
{
}

/*********************************************************************
 *                                                                   *
 *                          PUBLIC METHODS                           *
 *                                                                   *
 ********************************************************************/

/*------------------------------------- CClassParser::addMember()
 * addMember()
 *   Add a member to the structure.
 *
 * Parameters:
 *   aMember           The new member.
 *
 * Returns:
 *   -
 *-----------------------------------------------------------------*/
void CParsedStruct::addMember( CParsedAttribute *aMember )
{
  assert( aMember != NULL );

  members.insert( aMember->name, aMember );
}

/*------------------------------------ CClassParser::getMemberByName()
 * getMemberByName()
 *   Fetch a member by using its' name.
 *
 * Parameters:
 *   aName             The name of the member.
 *
 * Returns:
 *   CParsedAttribute * The member.
 *   NULL              The member was not found.
 *-----------------------------------------------------------------*/
CParsedAttribute *CParsedStruct::getMemberByName( const char *aName )
{
  assert( aName != NULL );

  return members.find( aName );
}

/*------------------------------------------------- CClassStore::out()
 * out()
 *   Output this object to stdout.
 *
 * Parameters:
 *   -
 * Returns:
 *   -
 *-----------------------------------------------------------------*/
void CParsedStruct::out()
{
  cout << "   " << name << "\n";
  for( memberIterator.toFirst();
       memberIterator.current();
       ++memberIterator )
    memberIterator.current()->out();
}
