/* This file is part of KDevelop
   Copyright 2013 Milian Wolff <mail@milianw.de>

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

/**
 * "returnType" : { "toString"  : "const int" }
 */
const int f1();

/**
 * "returnType" : { "toString"  : "const int" }
 */
int const f2();

/**
 * "returnType" : { "toString"  : "const int&" }
 */
int const& f3();

/**
 * "returnType" : { "toString"  : "const int&" }
 */
const int& f4();

/**
 * "returnType" : { "toString"  : "const int*" }
 */
int const* f5();

/**
 * "returnType" : { "toString"  : "const int*" }
 */
const int* f6();

/**
 * "returnType" : { "toString"  : "const int* const" }
 */
int const* const f7();

/**
 * "returnType" : { "toString"  : "const int* const" }
 */
const int* const f8();
