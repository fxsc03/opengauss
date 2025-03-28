/* -------------------------------------------------------------------------
 *
 * extension.h
 *		Extension management commands (create/drop extension).
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/extension.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef EXTENSION_H
#define EXTENSION_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

/*
 * creating_extension is only true while running a CREATE EXTENSION command.
 * It instructs recordDependencyOnCurrentExtension() to register a dependency
 * on the current pg_extension object for each SQL object created by its
 * installation script.
 */
extern THR_LOCAL bool creating_extension;

extern ObjectAddress CreateExtension(CreateExtensionStmt* stmt);

extern void RemoveExtensionById(Oid extId);

extern ObjectAddress InsertExtensionTuple(const char* extName, Oid extOwner, Oid schemaOid, bool relocatable,
    const char* extVersion, Datum extConfig, Datum extCondition, List* requiredExtensions);

extern ObjectAddress ExecAlterExtensionStmt(AlterExtensionStmt* stmt);

extern ObjectAddress ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt* stmt, ObjectAddress *objAddr=NULL);

extern Oid get_extension_oid(const char* extname, bool missing_ok);
extern char* get_extension_name(Oid ext_oid);
extern Oid get_extension_schema(Oid ext_oid);

extern ObjectAddress AlterExtensionNamespace(List* names, const char* newschema);

extern void AlterExtensionOwner_oid(Oid extensionOid, Oid newOwnerId);

/* Retuen true if the extension is supported. */
extern bool CheckExtensionValid(const char *extentName);
extern bool CheckExtensionSqlValid(char *queryString);

extern void RepallocSessionVarsArrayIfNecessary();
extern bool CheckIfExtensionExists(const char* extname);
extern bool IsFileExisted(const char *filename);
extern void ExecuteFunctionIfExisted(const char *filename, char *funcname);
#endif /* EXTENSION_H */
