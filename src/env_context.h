/*
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#ifndef CFENGINE_ENV_CONTEXT_H
#define CFENGINE_ENV_CONTEXT_H

#include "cf3.defs.h"

#include "alphalist.h"

/**
  The global heap
  Classes are added to the global heap using NewClass().
  */
extern AlphaList VHEAP;

/**
  Negated classes
  Classes may be negated by using the command line option ‘-N’ or by being cancelled during
  exit status of a classes body: cancel ̇kept etc.
  */
extern Item *VNEGHEAP;

/**
  The bundle heap
  Classes are added to a local bundle heap using NewBundleClass().
  */
extern AlphaList VADDCLASSES;

/* - Parsing/evaluating expressions - */
void ValidateClassSyntax(const char *str);
bool IsDefinedClass(const char *class);
bool IsExcluded(const char *exception);

bool EvalProcessResult(const char *process_result, AlphaList *proc_attr);
bool EvalFileResult(const char *file_result, AlphaList *leaf_attr);

/* - Rest - */
int Abort(void);
void AddAbortClass(const char *name, const char *classes);
void KeepClassContextPromise(Promise *pp);
void PushPrivateClassContext(void);
void PopPrivateClassContext(void);
void DeletePrivateClassContext(void);
void DeleteEntireHeap(void);
void NewPersistentContext(char *name, unsigned int ttl_minutes, enum statepolicy policy);
void DeletePersistentContext(char *name);
void LoadPersistentContext(void);
void AddEphemeralClasses(Rlist *classlist);
void NewClass(const char *oclass);      /* Copies oclass */
void NewBundleClass(char *class, char *bundle);
Rlist *SplitContextExpression(char *context, Promise *pp);
void DeleteClass(char *class);
int VarClassExcluded(Promise *pp, char **classes);
void NewClassesFromString(char *classlist);
void NegateClassesFromString(char *class);
bool IsSoftClass(char *sp);
bool IsHardClass(char *sp);
bool IsTimeClass(char *sp);
void SaveClassEnvironment(void);
void DeleteAllClasses(Rlist *list);
void AddAllClasses(Rlist *list, int persist, enum statepolicy policy);
void ListAlphaList(FILE *fp, AlphaList al, char sep);

#endif
