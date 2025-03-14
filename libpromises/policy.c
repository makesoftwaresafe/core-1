/*
  Copyright 2022 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

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
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <policy.h>

#include <syntax.h>
#include <string_lib.h>
#include <conversion.h>
#include <mutex.h>
#include <misc_lib.h>
#include <mod_custom.h>
#include <mod_files.h>
#include <vars.h>
#include <fncall.h>
#include <rlist.h>
#include <set.h>
#include <eval_context.h>
#include <promises.h>
#include <item_lib.h>
#include <hash.h>
#include <files_names.h>
#include <audit.h>
#include <logging.h>
#include <expand.h>

static const char *const POLICY_ERROR_BUNDLE_NAME_RESERVED =
    "Use of a reserved container name as a bundle name \"%s\"";
static const char *const POLICY_ERROR_BUNDLE_REDEFINITION =
    "Duplicate definition of bundle %s with type %s";
static const char *const POLICY_ERROR_BODY_REDEFINITION =
    "Duplicate definition of body %s with type %s";
static const char *const POLICY_ERROR_BODY_UNDEFINED =
    "Undefined body %s with type %s";
static const char *const POLICY_ERROR_BODY_CONTROL_ARGS =
    "Control bodies cannot take arguments, body %s control";
static const char *const POLICY_ERROR_PROMISE_UNCOMMENTED =
    "Promise is missing a comment attribute, and comments are required "
    "by policy";
static const char *const POLICY_ERROR_PROMISE_DUPLICATE_HANDLE =
    "Duplicate promise handle %s found";
static const char *const POLICY_ERROR_LVAL_INVALID =
    "Promise type %s has unknown attribute %s";
static const char *const POLICY_ERROR_PROMISE_ATTRIBUTE_NOT_IMPLEMENTED =
    "Common attribute '%s' not implemented for custom promises (%s)";
static const char *const POLICY_ERROR_PROMISE_ATTRIBUTE_NOT_SUPPORTED =
    "Common attribute '%s' not supported for custom promises, use '%s' instead (%s promises)";
static const char *const POLICY_ERROR_PROMISE_TYPE_UNSUPPORTED =
    "Promise type '%s' not supported by '%s' bundle type";

static const char *const POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH =
    "Type mismatch in constraint: %s";

static const char *const POLICY_ERROR_EMPTY_VARREF =
    "Empty variable reference";

//************************************************************************

static void BundleDestroy(Bundle *bundle);
static void BodyDestroy(Body *body);
static SyntaxTypeMatch ConstraintCheckType(const Constraint *cp);
static bool PromiseCheck(const Promise *pp, Seq *errors);

/*************************************************************************/

/**
 * @brief Return a default bundle name for this method/service
 */
Rval DefaultBundleConstraint(const Promise *pp, char *promisetype)
{
    static char name[CF_BUFSIZE];
    snprintf(name, CF_BUFSIZE, "%s_%s", promisetype, CanonifyName(pp->promiser));
    return (Rval) { name, RVAL_TYPE_SCALAR };
}

/*************************************************************************/

const char *NamespaceDefault(void)
{
    return "default";
}

/*************************************************************************/

Policy *PolicyNew(void)
{
    Policy *policy = xcalloc(1, sizeof(Policy));

    policy->release_id = NULL;
    policy->bundles = SeqNew(100, BundleDestroy);
    policy->bodies = SeqNew(100, BodyDestroy);
    policy->custom_promise_types = SeqNew(20, BodyDestroy);
    policy->policy_files_hashes = NULL;

    return policy;
}

/*************************************************************************/

int PolicyCompare(const void *a, const void *b)
{
    return a - b;
}

/*************************************************************************/

void PolicyDestroy(Policy *policy)
{
    if (policy != NULL)
    {
        SeqDestroy(policy->bundles);
        SeqDestroy(policy->bodies);
        SeqDestroy(policy->custom_promise_types);
        free(policy->release_id);
        if (policy->policy_files_hashes != NULL)
        {
            StringMapDestroy(policy->policy_files_hashes);
        }

        free(policy);
    }
}

/*************************************************************************/

static unsigned ConstraintHash(const Constraint *cp, unsigned seed)
{
    unsigned hash = seed;

    hash = StringHash(cp->lval, hash);
    hash = StringHash(cp->classes, hash);
    hash = RvalHash(cp->rval, hash);

    return hash;
}

/*************************************************************************/

static unsigned BodyHash(const Body *body, unsigned seed)
{
    unsigned hash = seed;
    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        const Constraint *cp = SeqAt(body->conlist, i);
        hash = ConstraintHash(cp, hash);
    }

    return hash;
}
/*************************************************************************/

static unsigned PromiseHash(const Promise *pp, unsigned seed)
{
    unsigned hash = seed;

    hash = StringHash(pp->promiser, seed);
    hash = RvalHash(pp->promisee, hash);

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        const Constraint *cp = SeqAt(pp->conlist, i);
        hash = ConstraintHash(cp, hash);
    }

    return hash;
}

/*************************************************************************/

static unsigned BundleSectionHash(const BundleSection *section, unsigned seed)
{
    unsigned hash = seed;

    hash = StringHash(section->promise_type, hash);
    for (size_t i = 0; i < SeqLength(section->promises); i++)
    {
        const Promise *pp = SeqAt(section->promises, i);
        hash = PromiseHash(pp, hash);
    }

    return hash;
}

/*************************************************************************/

static unsigned BundleHash(const Bundle *bundle, unsigned seed)
{
    unsigned hash = seed;

    hash = StringHash(bundle->type, hash);
    hash = StringHash(bundle->ns, hash);
    hash = StringHash(bundle->name, hash);
    hash = RlistHash(bundle->args, hash);

    for (size_t i = 0; i < SeqLength(bundle->sections); i++)
    {
        const BundleSection *section = SeqAt(bundle->sections, i);
        hash = BundleSectionHash(section, hash);
    }

    return hash;
}

/*************************************************************************/

unsigned PolicyHash(const Policy *policy)
{
    unsigned hash = 0;

    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        const Body *body = SeqAt(policy->bodies, i);
        hash = BodyHash(body, hash);
    }

    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        const Bundle *bundle = SeqAt(policy->bundles, i);
        hash = BundleHash(bundle, hash);
    }

    return hash;
}

/*************************************************************************/

StringSet *PolicySourceFiles(const Policy *policy)
{
    StringSet *files = StringSetNew();

    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        const Bundle *bp = SeqAt(policy->bundles, i);
        if (bp->source_path)
        {
            StringSetAdd(files, xstrdup(bp->source_path));
        }
    }

    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        const Bundle *bp = SeqAt(policy->bodies, i);
        if (bp->source_path)
        {
            StringSetAdd(files, xstrdup(bp->source_path));
        }
    }

    return files;
}

/*************************************************************************/

/**
 * Get hash digest of the given policy file.
 *
 * @param policy Policy that is supposed to contain (have loaded) the file
 * @param policy_file_path Absolute path of the policy file to get the digest for
 * @return Hash digest of the given policy file or %NULL if unknown
 * @note   The returned hash digest is owned by the policy, **do not free it**.
 */
const char *PolicyGetPolicyFileHash(const Policy *policy, const char *policy_file_path)
{
    return StringMapGet(policy->policy_files_hashes, policy_file_path);
}

/*************************************************************************/

static const char *StripNamespace(const char *full_symbol)
{
    char *sep = strchr(full_symbol, CF_NS);

    if (sep == NULL)
    {
        return full_symbol;
    }
    else
    {
        return sep + 1;
    }
}

/*************************************************************************/

/**
 * @brief Query a policy for a body
 * @param policy The policy to query
 * @param ns Namespace filter (optionally NULL)
 * @param type Body type filter
 * @param name Body name filter
 * @return Body child object if found, otherwise NULL
 */
Body *PolicyGetBody(const Policy *policy, const char *ns, const char *type, const char *name)
{
    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        Body *bp = SeqAt(policy->bodies, i);
        const char *body_symbol = StripNamespace(bp->name);

        if (strcmp(bp->type, type)    == 0 &&
            strcmp(body_symbol, name) == 0)
        {
            // allow namespace to be optionally matched
            if (ns && strcmp(bp->ns, ns) != 0)
            {
                continue;
            }

            return bp;
        }
    }

    return NULL;
}

/*************************************************************************/

/**
 * @brief Query a policy for a bundle
 * @param policy The policy to query
 * @param ns Namespace filter (optionally NULL)
 * @param type Bundle type filter
 * @param name Bundle name filter
 * @return Bundle child object if found, otherwise NULL
 */
Bundle *PolicyGetBundle(const Policy *policy, const char *ns, const char *type, const char *name)
{
    const char *bundle_symbol = StripNamespace(name);

    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        Bundle *bp = SeqAt(policy->bundles, i);

        if ((type == NULL || strcmp(bp->type, type) == 0)
            &&
            ((strcmp(bp->name, bundle_symbol) == 0) ||
             (strcmp(bp->name, name)          == 0)))
        {
            // allow namespace to be optionally matched
            if (ns && strcmp(bp->ns, ns) != 0)
            {
                continue;
            }

            return bp;
        }
    }

    return NULL;
}

/*************************************************************************/

/**
 * @brief Check to see if a policy is runnable (contains body common control)
 * @param policy Policy to check
 * @return True if policy is runnable
 */
bool PolicyIsRunnable(const Policy *policy)
{
    return PolicyGetBody(policy, NULL, "common", "control") != NULL;
}

/*************************************************************************/

/**
 * @brief Merge two partial policy objects. The memory for the child objects of the original policies are transferred to the new parent.
 * @param a
 * @param b
 * @return Merged policy
 */
Policy *PolicyMerge(Policy *a, Policy *b)
{
    assert(a != NULL);
    assert(b != NULL);

    Policy *result = PolicyNew();

    SeqAppendSeq(result->bundles, a->bundles);
    SeqSoftDestroy(a->bundles);
    SeqAppendSeq(result->bundles, b->bundles);
    SeqSoftDestroy(b->bundles);

    for (size_t i = 0; i < SeqLength(result->bundles); i++)
    {
        Bundle *bp = SeqAt(result->bundles, i);
        bp->parent_policy = result;
    }

    SeqAppendSeq(result->bodies, a->bodies);
    SeqSoftDestroy(a->bodies);
    SeqAppendSeq(result->bodies, b->bodies);
    SeqSoftDestroy(b->bodies);

    for (size_t i = 0; i < SeqLength(result->bodies); i++)
    {
        Body *bdp = SeqAt(result->bodies, i);
        bdp->parent_policy = result;
    }

    SeqAppendSeq(result->custom_promise_types, a->custom_promise_types);
    SeqSoftDestroy(a->custom_promise_types);
    SeqAppendSeq(result->custom_promise_types, b->custom_promise_types);
    SeqSoftDestroy(b->custom_promise_types);

    for (size_t i = 0; i < SeqLength(result->custom_promise_types); i++)
    {
        Body *bdp = SeqAt(result->custom_promise_types, i);
        bdp->parent_policy = result;
    }

    StringMap *extra_hashes = NULL;
    if (a->policy_files_hashes != NULL)
    {
        result->policy_files_hashes = a->policy_files_hashes;
        a->policy_files_hashes = NULL;
        extra_hashes = b->policy_files_hashes;
        b->policy_files_hashes = NULL;
    }
    else if (b->policy_files_hashes != NULL)
    {
        result->policy_files_hashes = b->policy_files_hashes;
        b->policy_files_hashes = NULL;
    }
    else
    {
        result->policy_files_hashes = NULL;
    }

    if (extra_hashes != NULL)
    {
        MapIterator it = MapIteratorInit(extra_hashes->impl);
        MapKeyValue *item;
        while ((item = MapIteratorNext(&it)) != NULL)
        {
            /* Move data and duplicate just the keys (which are always owned by
               the map). */
            StringMapInsert(result->policy_files_hashes,
                            xstrdup((char*) item->key), (char*) item->value);
        }
        /* Destroy only the map and the keys, data was moved. */
        StringMapSoftDestroy(extra_hashes);
    }

    /* Should result take over a release_id ? */
    free(a->release_id);
    free(b->release_id);
    free(a);
    free(b);

    return result;
}

/*************************************************************************/

const char *ConstraintGetNamespace(const Constraint *cp)
{
    switch (cp->type)
    {
    case POLICY_ELEMENT_TYPE_BODY:
        return cp->parent.body->ns;

    case POLICY_ELEMENT_TYPE_PROMISE:
        return cp->parent.promise->parent_section->parent_bundle->ns;

    default:
        ProgrammingError("Constraint has parent type: %d", cp->type);
    }
}

/*************************************************************************/

/**
 * @brief Convenience function to get the policy object associated with a promise
 * @param promise
 * @return Policy object
 */
const Policy *PolicyFromPromise(const Promise *promise)
{
    assert(promise);

    BundleSection *section = promise->parent_section;
    assert(section != NULL);

    Bundle *bundle = section->parent_bundle;
    assert(bundle);

    return bundle->parent_policy;
}

char *BundleQualifiedName(const Bundle *bundle)
{
    assert(bundle);
    if (!bundle)
    {
        return NULL;
    }

    if (bundle->name)
    {
        const char *ns = bundle->ns ? bundle->ns : NamespaceDefault();
        return StringConcatenate(3, ns, ":", bundle->name);  // CF_NS == ':'
    }

    return NULL;
}

static bool RvalTypeCheckDataType(RvalType rval_type, DataType expected_datatype)
{
    if (rval_type == RVAL_TYPE_FNCALL)
    {
        return true;
    }

    switch (expected_datatype)
    {
    case CF_DATA_TYPE_BODY:
    case CF_DATA_TYPE_BUNDLE:
        return rval_type == RVAL_TYPE_SCALAR;

    case CF_DATA_TYPE_CONTEXT:
    case CF_DATA_TYPE_COUNTER:
    case CF_DATA_TYPE_INT:
    case CF_DATA_TYPE_INT_RANGE:
    case CF_DATA_TYPE_OPTION:
    case CF_DATA_TYPE_REAL:
    case CF_DATA_TYPE_REAL_RANGE:
    case CF_DATA_TYPE_STRING:
        return rval_type == RVAL_TYPE_SCALAR;

    case CF_DATA_TYPE_CONTEXT_LIST:
    case CF_DATA_TYPE_INT_LIST:
    case CF_DATA_TYPE_OPTION_LIST:
    case CF_DATA_TYPE_REAL_LIST:
    case CF_DATA_TYPE_STRING_LIST:
        return (rval_type == RVAL_TYPE_SCALAR) || (rval_type == RVAL_TYPE_LIST);

    case CF_DATA_TYPE_CONTAINER:
        return (rval_type == RVAL_TYPE_CONTAINER);

    default:
        ProgrammingError("Unhandled expected datatype in switch: %d", expected_datatype);
    }
}

/*************************************************************************/

/* Check if a constraint's syntax is correct according to its promise_type and
   lvalue.
*/
static bool ConstraintCheckSyntax(const Constraint *constraint, Seq *errors)
{
    assert(constraint != NULL);
    if (constraint->type != POLICY_ELEMENT_TYPE_PROMISE)
    {
        ProgrammingError("Attempted to check the syntax for a constraint"
                         " not belonging to a promise");
    }

    const BundleSection *section = constraint->parent.promise->parent_section;
    const Bundle *bundle = section->parent_bundle;

    /* Check if lvalue is valid for the bundle's specific section. */
    const PromiseTypeSyntax *promise_type_syntax = PromiseTypeSyntaxGet(bundle->type, section->promise_type);
    for (size_t i = 0; promise_type_syntax->constraints[i].lval != NULL; i++)
    {
        const ConstraintSyntax *body_syntax = &promise_type_syntax->constraints[i];
        if (strcmp(body_syntax->lval, constraint->lval) == 0)
        {
            if (!RvalTypeCheckDataType(constraint->rval.type, body_syntax->dtype))
            {
                SeqAppend(errors,
                          PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                                         POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH, constraint->lval));
                return false;
            }
            return true;
        }
    }
    /* FIX: Call a VerifyConstraint() hook for the specific promise_type, defined
       in verify_TYPE.c, that checks for promise_type-specific constraint syntax. */

    /* Check if lvalue is valid for all bodies. */
    for (size_t i = 0; CF_COMMON_BODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_BODIES[i].lval) == 0)
        {
            if (!RvalTypeCheckDataType(constraint->rval.type, CF_COMMON_BODIES[i].dtype))
            {
                SeqAppend(errors,
                          PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                                         POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH, constraint->lval));
                return false;
            }
            return true;
        }
    }
    for (size_t i = 0; CF_COMMON_EDITBODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_EDITBODIES[i].lval) == 0)
        {
            if (!RvalTypeCheckDataType(constraint->rval.type, CF_COMMON_EDITBODIES[i].dtype))
            {
                SeqAppend(errors,
                          PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                                         POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH, constraint->lval));
                return false;
            }
            return true;
        }
    }
    for (size_t i = 0; CF_COMMON_XMLBODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_XMLBODIES[i].lval) == 0)
        {
            if (!RvalTypeCheckDataType(constraint->rval.type, CF_COMMON_XMLBODIES[i].dtype))
            {
                SeqAppend(errors,
                          PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                                         POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH, constraint->lval));
                return false;
            }
            return true;
        }
    }

    /* lval is unknown for this promise type */
    SeqAppend(errors,
              PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                             POLICY_ERROR_LVAL_INVALID,
                             constraint->parent.promise->parent_section->promise_type,
                             constraint->lval));

    return false;
}

/*************************************************************************/

static bool PolicyCheckPromiseType(const BundleSection *section, Seq *errors)
{
    assert(section != NULL);
    assert(section->parent_bundle);
    bool success = true;

    for (size_t i = 0; i < SeqLength(section->promises); i++)
    {
        const Promise *pp = SeqAt(section->promises, i);
        success &= PromiseCheck(pp, errors);
    }

    return success;
}

/*************************************************************************/

static inline bool PolicyCheckBundleSections(Seq * sections, Seq *errors)
{
    bool success = true;
    const size_t length = SeqLength(sections);
    for (size_t i = 0; i < length; i++)
    {
        const BundleSection *section = SeqAt(sections, i);
        success &= PolicyCheckPromiseType(section, errors);
    }
    return success;
}

static bool PolicyCheckBundle(const Bundle *bundle, Seq *errors)
{
    assert(bundle);
    bool success = true;

    // ensure no reserved bundle names are used
    {
        static const char *const reserved_names[] =
            { "sys", "const", "mon", "edit", "match", "mon", "this", NULL };
        if (IsStrIn(bundle->name, reserved_names))
        {
            SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_BUNDLE, bundle,
                                             POLICY_ERROR_BUNDLE_NAME_RESERVED, bundle->name));
            success = false;
        }
    }

    success &= PolicyCheckBundleSections(bundle->sections, errors);
    success &= PolicyCheckBundleSections(bundle->custom_sections, errors);

    return success;
}

static bool PolicyCheckBody(const Body *body, Seq *errors)
{
    bool success = true;

    if (strcmp("control", body->name) == 0)
    {
        if (RlistLen(body->args) > 0)
        {
            SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_BODY, body,
                                             POLICY_ERROR_BODY_CONTROL_ARGS,
                                             body->type));
            success = false;
        }
    }

    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        Constraint *cp = SeqAt(body->conlist, i);
        SyntaxTypeMatch err = ConstraintCheckType(cp);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, cp,
                                             POLICY_ERROR_CONSTRAINT_TYPE_MISMATCH,
                                             cp->lval));
            success = false;
        }
    }

    const BodySyntax *body_syntax = BodySyntaxGet(PARSER_BLOCK_BODY, body->type);
    assert(body_syntax && "Should have been checked at parse time");
    if (body_syntax->check_body)
    {
        success &= body_syntax->check_body(body, errors);
    }

    return success;
}

/*************************************************************************/

/* Get the syntax of a constraint according to its promise_type and lvalue.
   Make sure you've already checked the constraint's validity.
*/
static const ConstraintSyntax *ConstraintGetSyntax(const Constraint *constraint)
{
    if (constraint->type != POLICY_ELEMENT_TYPE_PROMISE)
    {
        ProgrammingError("Attempted to get the syntax for a constraint not belonging to a promise");
    }

    const Promise *promise = constraint->parent.promise;
    const BundleSection *section = promise->parent_section;
    const Bundle *bundle = section->parent_bundle;

    const PromiseTypeSyntax *promise_type_syntax = PromiseTypeSyntaxGet(bundle->type, section->promise_type);

    /* Check if lvalue is valid for the bundle's specific section. */
    for (size_t i = 0; promise_type_syntax->constraints[i].lval != NULL; i++)
    {
        const ConstraintSyntax *body_syntax = &promise_type_syntax->constraints[i];
        if (strcmp(body_syntax->lval, constraint->lval) == 0)
        {
            return body_syntax;
        }
    }

    /* Check if lvalue is valid for all bodies. */
    for (size_t i = 0; CF_COMMON_BODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_BODIES[i].lval) == 0)
        {
            return &CF_COMMON_BODIES[i];
        }
    }
    for (size_t i = 0; CF_COMMON_EDITBODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_EDITBODIES[i].lval) == 0)
        {
            return &CF_COMMON_EDITBODIES[i];
        }
    }
    for (size_t i = 0; CF_COMMON_XMLBODIES[i].lval != NULL; i++)
    {
        if (strcmp(constraint->lval, CF_COMMON_XMLBODIES[i].lval) == 0)
        {
            return &CF_COMMON_XMLBODIES[i];
        }
    }

    /* Syntax must have been checked first during PolicyCheckPartial(). */
    ProgrammingError("ConstraintGetSyntax() was called for constraint with "
                     "invalid lvalue: %s", constraint->lval);
    return NULL;
}

/*************************************************************************/

/**
 * @return A reference to the full symbol value of the Rval regardless of type, e.g. "foo:bar""foo:bar"
 */
static const char *RvalFullSymbol(const Rval *rval)
{
    switch (rval->type)
    {
    case RVAL_TYPE_SCALAR:
        return rval->item;
        break;

    case RVAL_TYPE_FNCALL:
        return ((FnCall *)rval->item)->name;

    default:
        ProgrammingError("Cannot get full symbol value from Rval of type %c", rval->type);
        return NULL;
    }
}

/**
 * @return A copy of the namespace component of a qualified name, or NULL. e.g. "foo:bar" -> "foo"
 */
char *QualifiedNameNamespaceComponent(const char *qualified_name)
{
    if (strchr(qualified_name, CF_NS))
    {
        char ns[256] = { 0 };
        sscanf(qualified_name, "%255[^:]", ns);

        return xstrdup(ns);
    }
    else
    {
        return NULL;
    }
}

/**
 * @return A copy of the symbol compoent of a qualified name, or NULL. e.g. "foo:bar" -> "bar"
 */
char *QualifiedNameScopeComponent(const char *qualified_name)
{
    char *sep = strchr(qualified_name, CF_NS);
    if (sep)
    {
        return xstrdup(sep + 1);
    }
    else
    {
        return xstrdup(qualified_name);
    }
}

static bool PolicyCheckUndefinedBodies(const Policy *policy, Seq *errors)
{
    bool success = true;

    for (size_t bpi = 0; bpi < SeqLength(policy->bundles); bpi++)
    {
        Bundle *bundle = SeqAt(policy->bundles, bpi);

        for (size_t sti = 0; sti < SeqLength(bundle->sections); sti++)
        {
            BundleSection *section = SeqAt(bundle->sections, sti);

            for (size_t ppi = 0; ppi < SeqLength(section->promises); ppi++)
            {
                Promise *promise = SeqAt(section->promises, ppi);

                for (size_t cpi = 0; cpi < SeqLength(promise->conlist); cpi++)
                {
                    Constraint *constraint = SeqAt(promise->conlist, cpi);

                    const ConstraintSyntax *syntax = ConstraintGetSyntax(constraint);
                    if (syntax->dtype == CF_DATA_TYPE_BODY)
                    {
                        char *ns = QualifiedNameNamespaceComponent(RvalFullSymbol(&constraint->rval));
                        char *symbol = QualifiedNameScopeComponent(RvalFullSymbol(&constraint->rval));

                        Body *referenced_body = PolicyGetBody(policy, ns, constraint->lval, symbol);
                        if (!referenced_body)
                        {
                            SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, constraint,
                                                             POLICY_ERROR_BODY_UNDEFINED, symbol, constraint->lval));
                            success = false;
                        }

                        free(ns);
                        free(symbol);
                    }
                } // constraints
            } // promises
        } // promise_types
    } // bundles

    return success;
}

static bool PolicyCheckRequiredComments(const EvalContext *ctx, const Policy *policy, Seq *errors)
{
    const Body *common_control = PolicyGetBody(policy, NULL, "common", "control");
    if (common_control)
    {
        bool require_comments = ConstraintsGetAsBoolean(ctx, "require_comments", common_control->conlist);
        if (!require_comments)
        {
            return true;
        }

        bool success = true;

        for (size_t bpi = 0; bpi < SeqLength(policy->bundles); bpi++)
        {
            Bundle *bundle = SeqAt(policy->bundles, bpi);

            for (size_t sti = 0; sti < SeqLength(bundle->sections); sti++)
            {
                BundleSection *section = SeqAt(bundle->sections, sti);

                for (size_t ppi = 0; ppi < SeqLength(section->promises); ppi++)
                {
                    Promise *promise = SeqAt(section->promises, ppi);

                    bool promise_has_comment = false;
                    for (size_t cpi = 0; cpi < SeqLength(promise->conlist); cpi++)
                    {
                        Constraint *constraint = SeqAt(promise->conlist, cpi);

                        if (strcmp(constraint->lval, "comment") == 0)
                        {
                            promise_has_comment = true;
                            break;
                        }
                    } // constraints

                    if (!promise_has_comment)
                    {
                        SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, promise,
                                                         POLICY_ERROR_PROMISE_UNCOMMENTED));
                        success = false;
                    }
                } // promises
            } // promise_types
        } // bundles

        return success;
    }
    else
    {
        return true;
    }
}

bool PolicyCheckDuplicateHandles(const Policy *policy, Seq *errors)
{
    bool success = true;

    Map *recorded = MapNew(StringHash_untyped, StringEqual_untyped, NULL, NULL);

    for (size_t bpi = 0; bpi < SeqLength(policy->bundles); bpi++)
    {
        Bundle *bundle = SeqAt(policy->bundles, bpi);

        for (size_t sti = 0; sti < SeqLength(bundle->sections); sti++)
        {
            BundleSection *section = SeqAt(bundle->sections, sti);

            for (size_t ppi = 0; ppi < SeqLength(section->promises); ppi++)
            {
                Promise *promise = SeqAt(section->promises, ppi);
                const char *handle = PromiseGetHandle(promise);

                if (handle)
                {
                    if (IsCf3VarString(handle))
                    {
                        // can't check dynamic handles
                        continue;
                    }

                    const Promise *other_promise = MapGet(recorded, handle);
                    if (other_promise)
                    {
                        // Need to make this smarter by comparing parsed expressions for equivalency.
                        if (strcmp(promise->classes, other_promise->classes) == 0)
                        {
                            SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_PROMISE, promise,
                                                             POLICY_ERROR_PROMISE_DUPLICATE_HANDLE, handle));
                            success = false;
                        }
                    }
                    else
                    {
                        MapInsert(recorded, (void *)handle, (void *)promise);
                    }
                }
            }
        }
    }

    MapDestroy(recorded);

    return success;
}

/**
 * @brief Check a runnable policy DOM for errors
 * @param policy Policy to check
 * @param errors Sequence of PolicyError to append errors to
 * @param ignore_missing_bundles Whether to ignore missing bundle references
 * @return True if no new errors are found
 */
bool PolicyCheckRunnable(const EvalContext *ctx, const Policy *policy, Seq *errors)
{
    bool success = true;

    success &= PolicyCheckRequiredComments(ctx, policy, errors);
    success &= PolicyCheckUndefinedBodies(policy, errors);
    success &= PolicyCheckDuplicateHandles(policy, errors);

    return success;
}

/**
 * @brief Check a partial policy DOM for errors
 * @param policy Policy to check
 * @param errors Sequence of PolicyError to append errors to
 * @return True if no new errors are found
 */
bool PolicyCheckPartial(const Policy *policy, Seq *errors)
{
    bool success = true;

    // ensure bundle names are not duplicated
    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        Bundle *bp = SeqAt(policy->bundles, i);

        for (size_t j = 0; j < SeqLength(policy->bundles); j++)
        {
            Bundle *bp2 = SeqAt(policy->bundles, j);

            if (bp != bp2
                && strcmp(bp->type, bp2->type) == 0
                && strcmp(bp->ns, bp2->ns) == 0
                && strcmp(bp->name, bp2->name) == 0)
            {
                SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_BUNDLE, bp,
                                                 POLICY_ERROR_BUNDLE_REDEFINITION,
                                                 bp->name, bp->type));
                success = false;
            }
        }
    }

    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        Bundle *bp = SeqAt(policy->bundles, i);
        success &= PolicyCheckBundle(bp, errors);
    }


    // ensure body names are not duplicated
    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        const Body *bp = SeqAt(policy->bodies, i);

        for (size_t j = 0; j < SeqLength(policy->bodies); j++)
        {
            const Body *bp2 = SeqAt(policy->bodies, j);

            if (bp != bp2
                && strcmp(bp->type, bp2->type) == 0
                && strcmp(bp->ns, bp2->ns) == 0
                && strcmp(bp->name, bp2->name) == 0)
            {
                if (strcmp(bp->type,"file") != 0)
                {
                    SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_BODY, bp,
                                                     POLICY_ERROR_BODY_REDEFINITION,
                                                     bp->name, bp->type));
                    success = false;
                }
            }
        }
    }

    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        const Body *body = SeqAt(policy->bodies, i);
        success &= PolicyCheckBody(body, errors);

    }

    success &= PolicyCheckDuplicateHandles(policy, errors);

    return success;
}

/*************************************************************************/

PolicyError *PolicyErrorNew(PolicyElementType type, const void *subject, const char *error_msg, ...)
{
    PolicyError *error = xmalloc(sizeof(PolicyError));

    error->type = type;
    error->subject = subject;

    va_list args;
    va_start(args, error_msg);
    xvasprintf(&error->message, error_msg, args);
    va_end(args);

    return error;
}

/*************************************************************************/

void PolicyErrorDestroy(PolicyError *error)
{
    free(error->message);
    free(error);
}

/*************************************************************************/

static SourceOffset PolicyElementSourceOffset(PolicyElementType type, const void *element)
{
    assert(element);

    switch (type)
    {
    case POLICY_ELEMENT_TYPE_POLICY:
    {
        return (SourceOffset) { 0 };
    }

    case POLICY_ELEMENT_TYPE_BUNDLE:
    {
        const Bundle *bundle = (const Bundle *)element;
        return bundle->offset;
    }

    case POLICY_ELEMENT_TYPE_BODY:
    {
        const Body *body = (const Body *)element;
        return body->offset;
    }

    case POLICY_ELEMENT_TYPE_BUNDLE_SECTION:
    {
        const BundleSection *section = (const BundleSection *) element;
        return section->offset;
    }

    case POLICY_ELEMENT_TYPE_PROMISE:
    {
        const Promise *promise = (const Promise *)element;
        return promise->offset;
    }

    case POLICY_ELEMENT_TYPE_CONSTRAINT:
    {
        const Constraint *constraint = (const Constraint *)element;
        return constraint->offset;
    }

    default:
        assert(false && "Invalid policy element");
        return (SourceOffset) { 0 };
    }
}

/*************************************************************************/

static const char *PolicyElementSourceFile(PolicyElementType type, const void *element)
{
    assert(element);

    switch (type)
    {
    case POLICY_ELEMENT_TYPE_POLICY:
        return "";

    case POLICY_ELEMENT_TYPE_BUNDLE:
    {
        const Bundle *bundle = (const Bundle *)element;
        return bundle->source_path;
    }

    case POLICY_ELEMENT_TYPE_BODY:
    {
        const Body *body = (const Body *)element;
        return body->source_path;
    }

    case POLICY_ELEMENT_TYPE_BUNDLE_SECTION:
    {
        const BundleSection *section = (const BundleSection *) element;
        return PolicyElementSourceFile(POLICY_ELEMENT_TYPE_BUNDLE, section->parent_bundle);
    }

    case POLICY_ELEMENT_TYPE_PROMISE:
    {
        const Promise *promise = (const Promise *)element;
        return PolicyElementSourceFile(POLICY_ELEMENT_TYPE_BUNDLE_SECTION, promise->parent_section);
    }

    case POLICY_ELEMENT_TYPE_CONSTRAINT:
    {
        const Constraint *constraint = (const Constraint *)element;
        switch (constraint->type)
        {
        case POLICY_ELEMENT_TYPE_BODY:
            return PolicyElementSourceFile(POLICY_ELEMENT_TYPE_BODY, constraint->parent.body);

        case POLICY_ELEMENT_TYPE_PROMISE:
            return PolicyElementSourceFile(POLICY_ELEMENT_TYPE_PROMISE, constraint->parent.promise);

        default:
            assert(false && "Constraint has invalid parent element type");
            return NULL;
        }
    }

    default:
        assert(false && "Invalid policy element");
        return NULL;
    }
}

/*************************************************************************/

void PolicyErrorWrite(Writer *writer, const PolicyError *error)
{
    SourceOffset offset = PolicyElementSourceOffset(error->type, error->subject);
    const char *path = PolicyElementSourceFile(error->type, error->subject);

    // FIX: need to track columns in SourceOffset
    WriterWriteF(writer, "%s:%zu:%zu: error: %s\n", path, offset.line, (size_t)0, error->message);
}

static char *PolicyErrorToString(const PolicyError *error)
{
    SourceOffset offset = PolicyElementSourceOffset(error->type, error->subject);
    const char *path = PolicyElementSourceFile(error->type, error->subject);

    Writer *msg = StringWriter();
    WriterWriteF(msg, "%s:%zu:%zu: %s.",
                 path, offset.line,
                 (size_t)0, error->message);

    if (error->type == POLICY_ELEMENT_TYPE_CONSTRAINT)
    {
        const Constraint *cp = error->subject;
        WriterWrite(msg, " Given attribute value '");
        RvalWrite(msg, cp->rval);
        WriterWriteChar(msg, '\'');
    }

    return StringWriterClose(msg);
}

/*************************************************************************/

void BundleSectionDestroy(BundleSection *section)
{
    if (section != NULL)
    {
        SeqDestroy(section->promises);

        free(section->promise_type);
        free(section);
    }
}

Bundle *PolicyAppendBundle(Policy *policy,
                           const char *ns, const char *name, const char *type,
                           const Rlist *args, const char *source_path)
{
    Bundle *bundle = xcalloc(1, sizeof(Bundle));

    bundle->parent_policy = policy;

    SeqAppend(policy->bundles, bundle);

    bundle->name = xstrdup(name);
    bundle->type = xstrdup(type);
    bundle->ns = xstrdup(ns);
    bundle->args = RlistCopy(args);
    bundle->source_path = SafeStringDuplicate(source_path);
    bundle->sections = SeqNew(10, BundleSectionDestroy);
    bundle->custom_sections = SeqNew(10, BundleSectionDestroy);

    return bundle;
}

/*******************************************************************/

Body *PolicyAppendBody(Policy *policy, const char *ns, const char *name,
                       const char *type, Rlist *args, const char *source_path,
                       bool is_custom)
{
    Body *body = xcalloc(1, sizeof(Body));
    body->parent_policy = policy;

    SeqAppend(policy->bodies, body);

    body->name = xstrdup(name);
    body->type = xstrdup(type);
    body->ns = xstrdup(ns);
    body->args = RlistCopy(args);
    body->source_path = SafeStringDuplicate(source_path);
    body->conlist = SeqNew(10, ConstraintDestroy);
    body->is_custom = is_custom;

    // TODO: move to standard callback
    if (strcmp("service_method", body->name) == 0)
    {
        Rlist *bundle_args = NULL;
        RlistAppendRval(&bundle_args, RvalNew("$(this.promiser)", RVAL_TYPE_SCALAR));
        RlistAppendRval(&bundle_args, RvalNew("$(this.service_policy)", RVAL_TYPE_SCALAR));

        FnCall *service_bundle = FnCallNew("standard_services", bundle_args);
        BodyAppendConstraint(body, "service_bundle", (Rval) { service_bundle, RVAL_TYPE_FNCALL }, "any", false);
    }

    return body;
}

/*******************************************************************/

Body *PolicyAppendPromiseBlock(
    Policy *policy,
    const char *ns,
    const char *name,
    const char *type,
    Rlist *args,
    const char *source_path)
{
    assert(policy != NULL);

    Body *body = xcalloc(1, sizeof(Body));
    body->parent_policy = policy;

    SeqAppend(policy->custom_promise_types, body);

    body->name = xstrdup(name);
    body->type = xstrdup(type);
    body->ns = xstrdup(ns);
    body->args = RlistCopy(args);
    body->source_path = SafeStringDuplicate(source_path);
    body->conlist = SeqNew(10, ConstraintDestroy);

    return body;
}

/*******************************************************************/

BundleSection *BundleAppendSection(Bundle *bundle, const char *promise_type)
{
    if (bundle == NULL)
    {
        ProgrammingError("Attempt to add a type without a bundle");
    }

    // TODO: review SeqLookup
    for (size_t i = 0; i < SeqLength(bundle->sections); i++)
    {
        BundleSection *existing = SeqAt(bundle->sections, i);
        if (strcmp(existing->promise_type, promise_type) == 0)
        {
            return existing;
        }
    }
    for (size_t i = 0; i < SeqLength(bundle->custom_sections); i++)
    {
        BundleSection *existing = SeqAt(bundle->custom_sections, i);
        if (strcmp(existing->promise_type, promise_type) == 0)
        {
            return existing;
        }
    }

    BundleSection *section = xcalloc(1, sizeof(BundleSection));

    section->parent_bundle = bundle;
    section->promise_type = xstrdup(promise_type);
    section->promises = SeqNew(10, PromiseDestroy);

    if (IsBuiltInPromiseType(promise_type))
    {
        SeqAppend(bundle->sections, section);
    }
    else
    {
        SeqAppend(bundle->custom_sections, section);
    }

    return section;
}

/*******************************************************************/

Promise *BundleSectionAppendPromise(BundleSection *section, const char *promiser, Rval promisee, const char *classes, const char *varclasses)
{
    assert(promiser && "Missing promiser");
    assert(section != NULL && "Missing promise type");

    Promise *pp = xcalloc(1, sizeof(Promise));

    pp->promiser = xstrdup(promiser);

    if (classes && strlen(classes) > 0)
    {
        pp->classes = xstrdup(classes);
    }
    else
    {
        pp->classes = xstrdup("any");
    }

    SeqAppend(section->promises, pp);

    pp->parent_section = section;

    pp->promisee = promisee;
    pp->conlist = SeqNew(10, ConstraintDestroy);
    pp->org_pp = pp;

    if (varclasses != NULL)
    {
        PromiseAppendConstraint(pp, "ifvarclass", RvalNew(varclasses, RVAL_TYPE_SCALAR), true);
    }

    return pp;
}

static void BundleDestroy(Bundle *bundle)
{
    if (bundle != NULL)
    {
        free(bundle->name);
        free(bundle->type);
        free(bundle->ns);
        free(bundle->source_path);

        RlistDestroy(bundle->args);
        SeqDestroy(bundle->sections);
        SeqDestroy(bundle->custom_sections);

        free(bundle);
    }
}

static void BodyDestroy(Body *body)
{
    if (body)
    {
        free(body->name);
        free(body->type);
        free(body->ns);
        free(body->source_path);

        RlistDestroy(body->args);
        SeqDestroy(body->conlist);
        free(body);
    }
}


void PromiseDestroy(Promise *pp)
{
    if (pp)
    {
        free(pp->promiser);

        if (pp->promisee.item)
        {
            RvalDestroy(pp->promisee);
        }

        free(pp->classes);
        free(pp->comment);

        SeqDestroy(pp->conlist);

        free(pp);
    }
}

/*******************************************************************/

static Constraint *ConstraintNew(const char *lval, Rval rval, const char *classes, bool references_body)
{
    Constraint *cp = xcalloc(1, sizeof(Constraint));

    cp->lval = SafeStringDuplicate(lval);
    cp->rval = rval;

    cp->classes = SafeStringDuplicate(classes);
    cp->references_body = references_body;

    return cp;
}

Constraint *PromiseAppendConstraint(Promise *pp, const char *lval, Rval rval, bool references_body)
{
    Constraint *cp = ConstraintNew(lval, rval, "any", references_body);
    cp->type = POLICY_ELEMENT_TYPE_PROMISE;
    cp->parent.promise = pp;

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *old_cp = SeqAt(pp->conlist, i);
        if (strcmp(old_cp->lval, lval) == 0)
        {
            if (strcmp(old_cp->lval, "ifvarclass") == 0 ||
                strcmp(old_cp->lval, "if") == 0)
            {
                // merge two if/ifvarclass promise attributes this
                // only happens in a variable context when we have a
                // scalar already in the attribute (old_cp)
                switch (rval.type)
                {
                case RVAL_TYPE_FNCALL: // case 1: merge FnCall with scalar
                {
                    char * rval_string = RvalToString(old_cp->rval);
                    Log(LOG_LEVEL_DEBUG, "PromiseAppendConstraint: merging PREVIOUS %s string context rval %s", old_cp->lval, rval_string);
                    Log(LOG_LEVEL_DEBUG, "PromiseAppendConstraint: merging NEW %s rval %s", old_cp->lval, rval_string);
                    free(rval_string);

                    Rlist *synthetic_args = NULL;
                    RlistAppendScalar(&synthetic_args, RvalScalarValue(old_cp->rval));

                    // append the old Rval (a function call) under the arguments of the new one
                    RlistAppend(&synthetic_args, rval.item, RVAL_TYPE_FNCALL);

                    Rval replacement = (Rval) { FnCallNew("and", synthetic_args), RVAL_TYPE_FNCALL };
                    rval_string = RvalToString(replacement);
                    Log(LOG_LEVEL_DEBUG, "PromiseAppendConstraint: MERGED %s rval %s", old_cp->lval, rval_string);
                    free(rval_string);

                    // overwrite the old Constraint rval with its replacement
                    RvalDestroy(cp->rval);
                    cp->rval = replacement;
                }
                break;

                case RVAL_TYPE_SCALAR:  // case 2: merge scalar with scalar
                {
                    Buffer *grow = BufferNew();
                    BufferAppendF(grow, "(%s).(%s)",
                                  RvalScalarValue(old_cp->rval),
                                  RvalScalarValue(rval));
                    RvalDestroy(cp->rval);
                    rval = RvalNew(BufferData(grow), RVAL_TYPE_SCALAR);
                    BufferDestroy(grow);
                    cp->rval = rval;
                }
                break;

                default:
                    ProgrammingError("PromiseAppendConstraint: unexpected rval type: %c", rval.type);
                    break;
                }
            }
            SeqSet(pp->conlist, i, cp);
            return cp;
        }
    }

    SeqAppend(pp->conlist, cp);
    return cp;
}

Constraint *BodyAppendConstraint(Body *body, const char *lval, Rval rval, const char *classes,
                                 bool references_body)
{
    Constraint *cp = ConstraintNew(lval, rval, classes, references_body);
    cp->type = POLICY_ELEMENT_TYPE_BODY;
    cp->parent.body = body;

    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        Constraint *old_cp = SeqAt(body->conlist, i);
        if (strcmp(old_cp->lval, lval) == 0 && strcmp(old_cp->classes, classes) == 0)
        {
            SeqSet(body->conlist, i, cp);
            return cp;
        }
    }

    SeqAppend(body->conlist, cp);

    return cp;
}

/*******************************************************************/

const BundleSection *BundleGetSection(const Bundle *bp, const char *promise_type)
{
    // TODO: hiding error, remove and see what will crash
    if (bp == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < SeqLength(bp->sections); i++)
    {
        BundleSection *sp = SeqAt(bp->sections, i);

        if (strcmp(promise_type, sp->promise_type) == 0)
        {
            return sp;
        }
    }

    return NULL;
}

/****************************************************************************/

static Buffer *EscapeQuotes(const char *raw, Buffer *out)
{
    const char *spf;

    for (spf = raw; *spf != '\0'; spf++)
    {
        switch (*spf)
        {
        case '\'':
        case '\"':
            BufferAppendChar(out, '\\');
            break;

        default:
            break;
        }
        BufferAppendChar(out, *spf);
    }

    return out;
}

/**
 * Converts the given attribute rval to a JSON object.
 *
 * @return A JsonElement of type JSON_ELEMENT_TYPE_CONTAINER
 */
static JsonElement *AttributeValueToJson(Rval rval, bool symbolic_reference)
{
    switch (rval.type)
    {
    case RVAL_TYPE_CONTAINER:
    {
        return JsonCopy(RvalContainerValue(rval));
    }

    case RVAL_TYPE_SCALAR:
    {
        Buffer *buffer = BufferNewWithCapacity(strlen(rval.item));

        EscapeQuotes((const char *) rval.item, buffer);

        JsonElement *json_attribute = JsonObjectCreate(10);

        if (symbolic_reference)
        {
            JsonObjectAppendString(json_attribute, "type", "symbol");
        }
        else
        {
            JsonObjectAppendString(json_attribute, "type", "string");
        }
        JsonObjectAppendString(json_attribute, "value", BufferData(buffer));

        BufferDestroy(buffer);

        return json_attribute;
    }


    case RVAL_TYPE_LIST:
    {
        Rlist *rp = NULL;
        JsonElement *list = JsonArrayCreate(10);

        JsonElement *json_attribute = JsonObjectCreate(10);
        JsonObjectAppendString(json_attribute, "type", "list");

        for (rp = (Rlist *) rval.item; rp != NULL; rp = rp->next)
        {
            JsonArrayAppendObject(list, AttributeValueToJson(rp->val, false));
        }

        JsonObjectAppendArray(json_attribute, "value", list);
        return json_attribute;
    }

    case RVAL_TYPE_FNCALL:
    {
        Rlist *argp = NULL;
        FnCall *call = (FnCall *) rval.item;

        JsonElement *json_attribute = JsonObjectCreate(10);
        JsonObjectAppendString(json_attribute, "type", "functionCall");
        JsonObjectAppendString(json_attribute, "name", call->name);

        {
            JsonElement *arguments = JsonArrayCreate(10);

            for (argp = call->args; argp != NULL; argp = argp->next)
            {
                JsonArrayAppendObject(arguments, AttributeValueToJson(argp->val, false));
            }

            JsonObjectAppendArray(json_attribute, "arguments", arguments);
        }

        return json_attribute;
    }

    case RVAL_TYPE_NOPROMISEE:
        ProgrammingError("Attempted to export attribute of type: %c", rval.type);
        return NULL;
    }

    assert(false);
    return NULL;
}

static JsonElement *CreateContextAsJson(const char *name, const char *children_name, JsonElement *children)
{
    JsonElement *json = JsonObjectCreate(10);

    JsonObjectAppendString(json, "name", name);
    JsonObjectAppendArray(json, children_name, children);

    return json;
}

static JsonElement *BodyContextsToJson(const Seq *constraints)
{
    JsonElement *json_contexts = JsonArrayCreate(10);
    JsonElement *json_attributes = JsonArrayCreate(10);
    char *current_context = "any";

    for (size_t i = 0; i < SeqLength(constraints); i++)
    {
        Constraint *cp = SeqAt(constraints, i);

        JsonElement *json_attribute = JsonObjectCreate(10);

        if (strcmp(current_context, cp->classes) != 0)
        {
            JsonArrayAppendObject(json_contexts,
                                  CreateContextAsJson(current_context,
                                                      "attributes", json_attributes));
            json_attributes = JsonArrayCreate(10);
            current_context = cp->classes;
        }

        JsonObjectAppendInteger(json_attribute, "line", cp->offset.line);

        JsonObjectAppendString(json_attribute, "lval", cp->lval);
        JsonObjectAppendObject(json_attribute, "rval", AttributeValueToJson(cp->rval, false));
        JsonArrayAppendObject(json_attributes, json_attribute);
    }

    JsonArrayAppendObject(json_contexts,
                          CreateContextAsJson(current_context,
                                              "attributes", json_attributes));

    return json_contexts;
}

static JsonElement *BundleContextsToJson(const Seq *promises)
{
    JsonElement *json_contexts = JsonArrayCreate(10);
    JsonElement *json_promises = JsonArrayCreate(10);
    char *current_context = NULL;

    for (size_t ppi = 0; ppi < SeqLength(promises); ppi++)
    {
        Promise *pp = SeqAt(promises, ppi);

        if (!current_context)
        {
            current_context = pp->classes;
        }

        JsonElement *json_promise = JsonObjectCreate(10);

        if (strcmp(current_context, pp->classes) != 0)
        {
            JsonArrayAppendObject(json_contexts,
                                  CreateContextAsJson(current_context,
                                                      "promises", json_promises));
            json_promises = JsonArrayCreate(10);
            current_context = pp->classes;
        }

        JsonObjectAppendInteger(json_promise, "line", pp->offset.line);

        {
            JsonElement *json_promise_attributes = JsonArrayCreate(10);

            for (size_t k = 0; k < SeqLength(pp->conlist); k++)
            {
                Constraint *cp = SeqAt(pp->conlist, k);

                JsonElement *json_attribute = JsonObjectCreate(10);

                JsonObjectAppendInteger(json_attribute, "line", cp->offset.line);

                JsonObjectAppendString(json_attribute, "lval", cp->lval);
                JsonElement *json_rval = AttributeValueToJson(cp->rval, cp->references_body);
                if (JsonGetContainerType(json_rval) == JSON_CONTAINER_TYPE_ARRAY)
                {
                    JsonObjectAppendArray(json_attribute, "rval", json_rval);
                }
                else
                {
                    JsonObjectAppendObject(json_attribute, "rval", json_rval);
                }
                JsonArrayAppendObject(json_promise_attributes, json_attribute);
            }

            JsonObjectAppendString(json_promise, "promiser", pp->promiser);

            switch (pp->promisee.type)
            {
            case RVAL_TYPE_SCALAR:
                JsonObjectAppendString(json_promise, "promisee", pp->promisee.item);
                break;

            case RVAL_TYPE_LIST:
            {
                JsonElement *promisee_list = JsonArrayCreate(10);
                for (const Rlist *rp = pp->promisee.item; rp; rp = rp->next)
                {
                    JsonArrayAppendString(promisee_list, RlistScalarValue(rp));
                }
                JsonObjectAppendArray(json_promise, "promisee", promisee_list);
            }
            break;

            default:
                break;
            }

            JsonObjectAppendArray(json_promise, "attributes", json_promise_attributes);
        }
        JsonArrayAppendObject(json_promises, json_promise);
    }

    if (JsonLength(json_promises) > 0)
    {
        JsonArrayAppendObject(json_contexts,
                              CreateContextAsJson(current_context,
                                                  "promises", json_promises));
    }

    return json_contexts;
}

/**
 * @brief Serialize a bundle as JSON
 * @param bundle The bundle to serialize
 * @return A JsonElement representing the input bundle
 */
JsonElement *BundleToJson(const Bundle *bundle)
{
    JsonElement *json_bundle = JsonObjectCreate(10);

    if (bundle->source_path)
    {
        JsonObjectAppendString(json_bundle, "sourcePath", bundle->source_path);
    }
    JsonObjectAppendInteger(json_bundle, "line", bundle->offset.line);

    JsonObjectAppendString(json_bundle, "namespace", bundle->ns);
    JsonObjectAppendString(json_bundle, "name", bundle->name);
    JsonObjectAppendString(json_bundle, "bundleType", bundle->type);

    {
        JsonElement *json_args = JsonArrayCreate(10);
        Rlist *argp = NULL;

        for (argp = bundle->args; argp != NULL; argp = argp->next)
        {
            JsonArrayAppendString(json_args, RlistScalarValue(argp));
        }

        JsonObjectAppendArray(json_bundle, "arguments", json_args);
    }

    {
        JsonElement *json_promise_types = JsonArrayCreate(10);

        for (size_t i = 0; i < SeqLength(bundle->sections); i++)
        {
            const BundleSection *sp = SeqAt(bundle->sections, i);

            JsonElement *json_promise_type = JsonObjectCreate(10);

            JsonObjectAppendInteger(json_promise_type, "line", sp->offset.line);
            JsonObjectAppendString(json_promise_type, "name", sp->promise_type);
            JsonObjectAppendArray(json_promise_type, "contexts", BundleContextsToJson(sp->promises));

            JsonArrayAppendObject(json_promise_types, json_promise_type);
        }

        JsonObjectAppendArray(json_bundle, "promiseTypes", json_promise_types);
    }

    return json_bundle;
}

/**
 * @brief Serialize a body as JSON
 * @param body The body to serialize
 * @return A JsonElement representing the input body
 */
JsonElement *BodyToJson(const Body *body)
{
    JsonElement *json_body = JsonObjectCreate(10);

    if (body->source_path)
    {
        JsonObjectAppendString(json_body, "sourcePath", body->source_path);
    }
    JsonObjectAppendInteger(json_body, "line", body->offset.line);

    JsonObjectAppendString(json_body, "namespace", body->ns);
    JsonObjectAppendString(json_body, "name", body->name);
    JsonObjectAppendString(json_body, "bodyType", body->type);

    {
        JsonElement *json_args = JsonArrayCreate(10);
        Rlist *argp = NULL;

        for (argp = body->args; argp != NULL; argp = argp->next)
        {
            JsonArrayAppendString(json_args, RlistScalarValue(argp));
        }

        JsonObjectAppendArray(json_body, "arguments", json_args);
    }

    JsonObjectAppendArray(json_body, "contexts", BodyContextsToJson(body->conlist));

    return json_body;
}

/**
 * @brief Serialize a policy as JSON
 * @param policy The policy to serialize
 * @return A JsonElement representing the input policy
 */
JsonElement *PolicyToJson(const Policy *policy)
{
    JsonElement *json_policy = JsonObjectCreate(10);

    {
        JsonElement *json_bundles = JsonArrayCreate(10);

        for (size_t i = 0; i < SeqLength(policy->bundles); i++)
        {
            const Bundle *bp = SeqAt(policy->bundles, i);
            JsonArrayAppendObject(json_bundles, BundleToJson(bp));
        }

        JsonObjectAppendArray(json_policy, "bundles", json_bundles);
    }

    {
        JsonElement *json_bodies = JsonArrayCreate(10);

        for (size_t i = 0; i < SeqLength(policy->bodies); i++)
        {
            const Body *bdp = SeqAt(policy->bodies, i);

            JsonArrayAppendObject(json_bodies, BodyToJson(bdp));
        }

        JsonObjectAppendArray(json_policy, "bodies", json_bodies);
    }

    return json_policy;
}

/****************************************************************************/


static void IndentPrint(Writer *writer, int indent_level)
{
    static const int PRETTY_PRINT_SPACES_PER_INDENT = 2;

    int i = 0;

    for (i = 0; i < PRETTY_PRINT_SPACES_PER_INDENT * indent_level; i++)
    {
        WriterWriteChar(writer, ' ');
    }
}

static void AttributeToString(Writer *writer, Constraint *attribute, bool symbolic_reference)
{
    WriterWriteF(writer, "%s => ", attribute->lval);
    if (symbolic_reference)
    {
        RvalWrite(writer, attribute->rval);
    }
    else
    {
        RvalWriteQuoted(writer, attribute->rval);
    }
}


static void ArgumentsToString(Writer *writer, Rlist *args)
{
    Rlist *argp = NULL;

    WriterWriteChar(writer, '(');
    for (argp = args; argp != NULL; argp = argp->next)
    {
        WriterWriteF(writer, "%s", RlistScalarValue(argp));

        if (argp->next != NULL)
        {
            WriterWrite(writer, ", ");
        }
    }
    WriterWriteChar(writer, ')');
}


void BodyToString(Writer *writer, Body *body)
{
    char *current_class = NULL;

    WriterWriteF(writer, "body %s %s", body->type, body->name);
    ArgumentsToString(writer, body->args);
    WriterWrite(writer, "\n{");

    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        Constraint *cp = SeqAt(body->conlist, i);

        if (current_class == NULL || strcmp(cp->classes, current_class) != 0)
        {
            current_class = cp->classes;

            if (strcmp(current_class, "any") == 0)
            {
                WriterWrite(writer, "\n");
            }
            else
            {
                WriterWriteF(writer, "\n\n%s::", current_class);
            }
        }

        IndentPrint(writer, 1);
        AttributeToString(writer, cp, false);
        WriterWriteChar(writer, ';');
        WriterWriteChar(writer, '\n');
    }

    WriterWrite(writer, "\n}\n");
}


void BundleToString(Writer *writer, Bundle *bundle)
{
    WriterWriteF(writer, "bundle %s %s", bundle->type, bundle->name);
    ArgumentsToString(writer, bundle->args);
    WriterWrite(writer, "\n{");

    for (size_t i = 0; i < SeqLength(bundle->sections); i++)
    {
        BundleSection *section = SeqAt(bundle->sections, i);

        WriterWriteF(writer, "\n%s:\n", section->promise_type);

        char *current_class = NULL;
        for (size_t ppi = 0; ppi < SeqLength(section->promises); ppi++)
        {
            Promise *pp = SeqAt(section->promises, ppi);

            if (current_class == NULL || strcmp(pp->classes, current_class) != 0)
            {
                current_class = pp->classes;
                IndentPrint(writer, 1);
                WriterWriteF(writer, "%s::\n", current_class);
            }

            IndentPrint(writer, 2);
            ScalarWrite(writer, pp->promiser, true);

            /* FIX: add support
             *
             if (pp->promisee != NULL)
             {
             fprintf(out, "%s", pp->promisee);
             }
            */

            for (size_t k = 0; k < SeqLength(pp->conlist); k++)
            {
                Constraint *cp = SeqAt(pp->conlist, k);

                IndentPrint(writer, 4);
                AttributeToString(writer, cp, cp->references_body);
                if (k < SeqLength(pp->conlist)-1)
                {
                    WriterWriteChar(writer, ',');
                    WriterWriteChar(writer, '\n');
                }
            }
            WriterWriteChar(writer, ';');
            WriterWriteChar(writer, '\n');
        }

        if (i == (SeqLength(bundle->sections) - 1))
        {
            WriterWriteChar(writer, '\n');
        }
    }

    WriterWrite(writer, "\n}\n");
}

/**
 * @brief Pretty-print a policy
 * @param policy The policy to print
 * @param writer Writer to write into
 */
void PolicyToString(const Policy *policy, Writer *writer)
{
    for (size_t i = 0; i < SeqLength(policy->bundles); i++)
    {
        Bundle *bundle = SeqAt(policy->bundles, i);
        BundleToString(writer, bundle);
        WriterWriteChar(writer, '\n');
    }

    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        Body *body = SeqAt(policy->bodies, i);
        BodyToString(writer, body);
        WriterWriteChar(writer, '\n');
    }

}

//*****************************************************************************

static Rval RvalFromJson(JsonElement *json_rval)
{
    const char *type = JsonObjectGetAsString(json_rval, "type");

    if (strcmp("string", type) == 0 || strcmp("symbol", type) == 0)
    {
        const char *value = JsonObjectGetAsString(json_rval, "value");
        return ((Rval) { xstrdup(value), RVAL_TYPE_SCALAR });
    }
    else if (strcmp("list", type) == 0)
    {
        JsonElement *json_list = JsonObjectGetAsArray(json_rval, "value");
        Rlist *rlist = NULL;

        for (size_t i = 0; i < JsonLength(json_list); i++)
        {
            Rval list_value = RvalFromJson(JsonArrayGetAsObject(json_list, i));
            RlistAppend(&rlist, list_value.item, list_value.type);
            RvalDestroy(list_value);
        }

        return ((Rval) { rlist, RVAL_TYPE_LIST });
    }
    else if (strcmp("functionCall", type) == 0)
    {
        const char *name = JsonObjectGetAsString(json_rval, "name");
        JsonElement *json_args = JsonObjectGetAsArray(json_rval, "arguments");
        Rlist *args = NULL;

        for (size_t i = 0; i < JsonLength(json_args); i++)
        {
            JsonElement *json_arg = JsonArrayGetAsObject(json_args, i);
            Rval arg = RvalFromJson(json_arg);

            RlistAppend(&args, arg.item, arg.type);
            RvalDestroy(arg);
        }

        FnCall *fn = FnCallNew(name, args);

        return ((Rval) { fn, RVAL_TYPE_FNCALL });
    }
    else
    {
        ProgrammingError("Unexpected rval type: %s", type);
    }
}

static Constraint *PromiseAppendConstraintJson(Promise *promise, JsonElement *json_constraint)
{
    const char *lval = JsonObjectGetAsString(json_constraint, "lval");

    JsonElement *json_rval = JsonObjectGetAsObject(json_constraint, "rval");
    const char *type = JsonObjectGetAsString(json_rval, "type");

    Rval rval = RvalFromJson(json_rval);

    Constraint *cp = PromiseAppendConstraint(promise, lval, rval, (strcmp("symbol", type) == 0));

    return cp;
}

static Promise *BundleSectionAppendPromiseJson(BundleSection *section, JsonElement *json_promise, const char *context)
{
    const char *promiser = JsonObjectGetAsString(json_promise, "promiser");

    Promise *promise = BundleSectionAppendPromise(section, promiser, (Rval) { NULL, RVAL_TYPE_NOPROMISEE }, context, NULL);

    JsonElement *json_attributes = JsonObjectGetAsArray(json_promise, "attributes");
    for (size_t i = 0; i < JsonLength(json_attributes); i++)
    {
        JsonElement *json_attribute = JsonArrayGetAsObject(json_attributes, i);
        PromiseAppendConstraintJson(promise, json_attribute);
    }

    return promise;
}

static BundleSection *BundleAppendSectionJson(Bundle *bundle, JsonElement *json_promise_type)
{
    const char *name = JsonObjectGetAsString(json_promise_type, "name");

    BundleSection *section = BundleAppendSection(bundle, name);

    JsonElement *json_contexts = JsonObjectGetAsArray(json_promise_type, "contexts");
    for (size_t i = 0; i < JsonLength(json_contexts); i++)
    {
        JsonElement *json_context = JsonArrayGetAsObject(json_contexts, i);

        const char *context = JsonObjectGetAsString(json_context, "name");

        JsonElement *json_context_promises = JsonObjectGetAsArray(json_context, "promises");
        for (size_t j = 0; j < JsonLength(json_context_promises); j++)
        {
            JsonElement *json_promise = JsonArrayGetAsObject(json_context_promises, j);
            BundleSectionAppendPromiseJson(section, json_promise, context);
        }
    }

    return section;
}

static Bundle *PolicyAppendBundleJson(Policy *policy, JsonElement *json_bundle)
{
    const char *ns = JsonObjectGetAsString(json_bundle, "namespace");
    const char *name = JsonObjectGetAsString(json_bundle, "name");
    const char *type = JsonObjectGetAsString(json_bundle, "bundleType");
    const char *source_path = JsonObjectGetAsString(json_bundle, "sourcePath");

    Rlist *args = NULL;
    {
        JsonElement *json_args = JsonObjectGetAsArray(json_bundle, "arguments");
        for (size_t i = 0; i < JsonLength(json_args); i++)
        {
            RlistAppendScalar(&args, JsonArrayGetAsString(json_args, i));
        }
    }

    Bundle *bundle = PolicyAppendBundle(policy, ns, name, type, args, source_path);

    {
        JsonElement *json_promise_types = JsonObjectGetAsArray(json_bundle, "promiseTypes");
        for (size_t i = 0; i < JsonLength(json_promise_types); i++)
        {
            JsonElement *json_promise_type = JsonArrayGetAsObject(json_promise_types, i);
            BundleAppendSectionJson(bundle, json_promise_type);
        }
    }

    return bundle;
}

static Constraint *BodyAppendConstraintJson(Body *body, JsonElement *json_constraint, const char *context)
{
    const char *lval = JsonObjectGetAsString(json_constraint, "lval");

    JsonElement *json_rval = JsonObjectGetAsObject(json_constraint, "rval");
    const char *type = JsonObjectGetAsString(json_rval, "type");

    Rval rval = RvalFromJson(json_rval);

    Constraint *cp = BodyAppendConstraint(body, lval, rval, context, (strcmp("symbol", type) == 0));

    return cp;
}

static Body *PolicyAppendBodyJson(Policy *policy, JsonElement *json_body)
{
    const char *ns = JsonObjectGetAsString(json_body, "namespace");
    const char *name = JsonObjectGetAsString(json_body, "name");
    const char *type = JsonObjectGetAsString(json_body, "bodyType");
    const char *source_path = JsonObjectGetAsString(json_body, "sourcePath");

    Rlist *args = NULL;
    {
        JsonElement *json_args = JsonObjectGetAsArray(json_body, "arguments");
        for (size_t i = 0; i < JsonLength(json_args); i++)
        {
            RlistAppendScalar(&args, JsonArrayGetAsString(json_args, i));
        }
    }

    Body *body = PolicyAppendBody(policy, ns, name, type, args, source_path, false);

    {
        JsonElement *json_contexts = JsonObjectGetAsArray(json_body, "contexts");
        for (size_t i = 0; i < JsonLength(json_contexts); i++)
        {
            JsonElement *json_context = JsonArrayGetAsObject(json_contexts, i);
            const char *context = JsonObjectGetAsString(json_context, "name");

            {
                JsonElement *json_attributes = JsonObjectGetAsArray(json_context, "attributes");
                for (size_t j = 0; j < JsonLength(json_attributes); j++)
                {
                    JsonElement *json_attribute = JsonArrayGetAsObject(json_attributes, j);
                    BodyAppendConstraintJson(body, json_attribute, context);
                }
            }
        }
    }

    return body;
}

/**
 * @brief Deserialize a policy from JSON
 * @param json_policy JSON to deserialize
 * @return A policy DOM
 */
Policy *PolicyFromJson(JsonElement *json_policy)
{
    Policy *policy = PolicyNew();

    JsonElement *json_bundles = JsonObjectGetAsArray(json_policy, "bundles");
    JsonElement *json_bodies = JsonObjectGetAsArray(json_policy, "bodies");

    if ((json_bundles == NULL) && (json_bodies == NULL))
    {
        return NULL;
    }

    if (json_bundles != NULL)
    {
        for (size_t i = 0; i < JsonLength(json_bundles); i++)
        {
            JsonElement *json_bundle = JsonArrayGetAsObject(json_bundles, i);
            PolicyAppendBundleJson(policy, json_bundle);
        }
    }
    if (json_bodies != NULL)
    {
        for (size_t i = 0; i < JsonLength(json_bodies); i++)
        {
            JsonElement *json_body = JsonArrayGetAsObject(json_bodies, i);
            PolicyAppendBodyJson(policy, json_body);
        }
    }

    return policy;
}

/**
 * @brief A sequence of constraints matching the l-value.
 * @param body Body to query
 * @param lval l-value to match
 * @return Sequence of pointers to the constraints. Destroying it does not alter the DOM.
 */
Seq *BodyGetConstraint(Body *body, const char *lval)
{
    Seq *matches = SeqNew(5, NULL);

    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        Constraint *cp = SeqAt(body->conlist, i);
        if (strcmp(cp->lval, lval) == 0)
        {
            SeqAppend(matches, cp);
        }
    }

    return matches;
}

bool BodyHasConstraint(const Body *body, const char *lval)
{
    for (size_t i = 0; i < SeqLength(body->conlist); i++)
    {
        Constraint *cp = SeqAt(body->conlist, i);
        if (StringEqual(lval, cp->lval))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Get the context of the given constraint
 * @param cp
 * @return context. never returns NULL.
 */
const char *ConstraintContext(const Constraint *cp)
{
    switch (cp->type)
    {
    case POLICY_ELEMENT_TYPE_BODY:
        return cp->classes;

    case POLICY_ELEMENT_TYPE_BUNDLE:
        return cp->parent.promise->classes;

    default:
        ProgrammingError("Constraint had parent element type: %d", cp->type);
        return NULL;
    }
}

/**
 * @brief Returns the first effective constraint from a list of candidates, depending on evaluation state.
 * @param constraints The list of potential candidates
 * @return The effective constraint, or NULL if none are found.
 */
Constraint *EffectiveConstraint(const EvalContext *ctx, Seq *constraints)
{
    for (size_t i = 0; i < SeqLength(constraints); i++)
    {
        Constraint *cp = SeqAt(constraints, i);

        const char *context = ConstraintContext(cp);
        if (IsDefinedClass(ctx, context))
        {
            return cp;
        }
    }

    return NULL;
}

void ConstraintDestroy(Constraint *cp)
{
    if (cp)
    {
        RvalDestroy(cp->rval);
        free(cp->lval);
        free(cp->classes);

        free(cp);
    }
}

/*****************************************************************************/

/**
 * @brief Get the boolean value of the first effective constraint found matching, from a promise
 * @return true/false
 * @note Returns #false if no matching constraint is found
 */
int PromiseGetConstraintAsBoolean(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    return PromiseGetConstraintAsBooleanWithDefault(ctx, lval, pp, false, false);
}

/**
 * @brief Get the boolean value of the first effective constraint found matching, from a promise
 * @return true/false
 */
int PromiseGetConstraintAsBooleanWithDefault(const EvalContext *ctx, const char *lval, const Promise *pp,
                                             int default_val, bool with_warning)
{
    assert(pp != NULL);

    int retval = CF_UNDEFINED;

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp(cp->lval, lval) == 0)
        {
            if (IsDefinedClass(ctx, cp->classes))
            {
                if (retval != CF_UNDEFINED)
                {
                    Log(LOG_LEVEL_ERR, "Multiple '%s' (boolean) constraints break this promise", lval);
                    PromiseRef(LOG_LEVEL_ERR, pp);
                }
            }
            else
            {
                continue;
            }

            if (cp->rval.type != RVAL_TYPE_SCALAR)
            {
                Log(LOG_LEVEL_ERR, "Type mismatch on rhs - expected type %c for boolean constraint '%s'",
                    cp->rval.type, lval);
                PromiseRef(LOG_LEVEL_ERR, pp);
                FatalError(ctx, "Aborted");
            }

            if (strcmp(cp->rval.item, "true") == 0 || strcmp(cp->rval.item, "yes") == 0)
            {
                retval = true;
                continue;
            }

            if (strcmp(cp->rval.item, "false") == 0 || strcmp(cp->rval.item, "no") == 0)
            {
                retval = false;
            }
        }
    }

    if (retval == CF_UNDEFINED)
    {
        if (with_warning)
        {
            Log(LOG_LEVEL_WARNING,
                "Using the default value '%s' for attribute %s (promiser: %s, file: %s:%zd), please set it explicitly",
                default_val ? "true" : "false", lval,
                pp->promiser, PromiseGetBundle(pp)->source_path, pp->offset.line);
        }
        retval = default_val;
    }

    return retval;
}

/*****************************************************************************/

/**
 * @brief Get the trinary boolean value of the first effective constraint found matching
 * @param lval
 * @param constraints
 * @return True/false, or CF_UNDEFINED if not found
 */
int ConstraintsGetAsBoolean(const EvalContext *ctx, const char *lval, const Seq *constraints)
{
    int retval = CF_UNDEFINED;

    for (size_t i = 0; i < SeqLength(constraints); i++)
    {
        Constraint *cp = SeqAt(constraints, i);

        if (strcmp(cp->lval, lval) == 0)
        {
            if (IsDefinedClass(ctx, cp->classes))
            {
                if (retval != CF_UNDEFINED)
                {
                    Log(LOG_LEVEL_ERR, "Multiple '%s' (boolean) body constraints break this promise", lval);
                }
            }
            else
            {
                continue;
            }

            if (cp->rval.type != RVAL_TYPE_SCALAR)
            {
                Log(LOG_LEVEL_ERR, "Type mismatch - expected type %c for boolean constraint '%s'",
                    cp->rval.type, lval);
                FatalError(ctx, "Aborted");
            }

            if (strcmp(cp->rval.item, "true") == 0 || strcmp(cp->rval.item, "yes") == 0)
            {
                retval = true;
                continue;
            }

            if (strcmp(cp->rval.item, "false") == 0 || strcmp(cp->rval.item, "no") == 0)
            {
                retval = false;
            }
        }
    }

    if (retval == CF_UNDEFINED)
    {
        retval = false;
    }

    return retval;
}

/*****************************************************************************/

bool PromiseBundleOrBodyConstraintExists(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    int retval = CF_UNDEFINED;

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        const Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp(cp->lval, lval) == 0)
        {
            if (IsDefinedClass(ctx, cp->classes))
            {
                if (retval != CF_UNDEFINED)
                {
                    Log(LOG_LEVEL_ERR, "Multiple '%s' constraints break this promise", lval);
                    PromiseRef(LOG_LEVEL_ERR, pp);
                }
            }
            else
            {
                continue;
            }

            if (!(cp->rval.type == RVAL_TYPE_FNCALL || cp->rval.type == RVAL_TYPE_SCALAR))
            {
                Log(LOG_LEVEL_ERR,
                    "Anomalous type mismatch - type %c for bundle constraint '%s' did not match internals",
                    cp->rval.type, lval);
                PromiseRef(LOG_LEVEL_ERR, pp);
                FatalError(ctx, "Aborted");
            }

            return true;
        }
    }

    return false;
}

static bool CheckScalarNotEmptyVarRef(const char *scalar)
{
    return (strcmp("$()", scalar) != 0) && (strcmp("${}", scalar) != 0);
}

static bool ValidateCustomPromise(const Promise *pp, Seq *errors)
{
    assert(pp != NULL);
    assert(pp->parent_section != NULL);
    assert(errors != NULL);

    const char *const promise_type = PromiseGetPromiseType(pp);
    bool valid = true;
    Seq *attributes = pp->conlist;
    const size_t length = SeqLength(attributes);
    for (size_t i = 0; i < length; ++i)
    {
        Constraint *attribute = SeqAt(attributes, i);
        const char *name = attribute->lval;
        if (StringEqual(name, "ifvarclass"))
        {
            SeqAppend(
                errors,
                PolicyErrorNew(
                    POLICY_ELEMENT_TYPE_PROMISE,
                    pp,
                    POLICY_ERROR_PROMISE_ATTRIBUTE_NOT_SUPPORTED,
                    name,
                    "if",
                    promise_type));
            valid = false;
        } else if (StringEqual(name, "action_policy")
            || StringEqual(name, "expireafter")
            || StringEqual(name, "meta"))
        {
            // TODO: Remove 1 attribute at a time, test and fix.
            //       https://tracker.mender.io/browse/CFE-3392
            SeqAppend(
                errors,
                PolicyErrorNew(
                    POLICY_ELEMENT_TYPE_PROMISE,
                    pp,
                    POLICY_ERROR_PROMISE_ATTRIBUTE_NOT_IMPLEMENTED,
                    name,
                    promise_type));
            valid = false;
        }
    }

    // TODO: If we are running --full-check, spawn promise module and perform
    //       validate operation. https://tracker.mender.io/browse/CFE-3430

    return valid;
}

static bool PromiseCheck(const Promise *pp, Seq *errors)
{
    assert(pp != NULL);

    bool success = true;

    if (!CheckScalarNotEmptyVarRef(pp->promiser))
    {
        SeqAppend(errors, PolicyErrorNew(POLICY_ELEMENT_TYPE_PROMISE, pp,
                                         POLICY_ERROR_EMPTY_VARREF));
        success = false;
    }

    const bool is_builtin = IsBuiltInPromiseType(PromiseGetPromiseType(pp));

    const PromiseTypeSyntax *pts = PromiseTypeSyntaxGet(pp->parent_section->parent_bundle->type,
                                                        PromiseGetPromiseType(pp));

    if (is_builtin)
    {
        if(pts == NULL)
        {
            // No promise type syntax to check for a built in promise
            // This should be an error, for example if trying to use
            // methods promises outside agent bundles
            SeqAppend(errors,
                      PolicyErrorNew(POLICY_ELEMENT_TYPE_BUNDLE_SECTION, pp->parent_section,
                                     POLICY_ERROR_PROMISE_TYPE_UNSUPPORTED,
                                     pp->parent_section->promise_type,
                                     pp->parent_section->parent_bundle->type));
            return false;
        }

        // check if promise's constraints are valid
        for (size_t i = 0; i < SeqLength(pp->conlist); i++)
        {
            Constraint *constraint = SeqAt(pp->conlist, i);
            success &= ConstraintCheckSyntax(constraint, errors);
        }
    }

    if (pts == NULL)
    {
        assert(!is_builtin);
        success &= ValidateCustomPromise(pp, errors);
    }
    else if (pts->check_promise)
    {
        assert(is_builtin);
        success &= pts->check_promise(pp, errors);
    }

    return success;
}

const char *PromiseGetNamespace(const Promise *pp)
{
    return pp->parent_section->parent_bundle->ns;
}

const Bundle *PromiseGetBundle(const Promise *pp)
{
    return pp->parent_section->parent_bundle;
}

const Policy *PromiseGetPolicy(const Promise *pp)
{
    return PromiseGetBundle(pp)->parent_policy;
}

static void BundlePath(Writer *w, const Bundle *bp)
{
    WriterWriteChar(w, '/');
    WriterWrite(w, bp->ns);
    WriterWriteChar(w, '/');
    WriterWrite(w, bp->name);
}

static void PromiseTypePath(Writer *w, const BundleSection *section)
{
    BundlePath(w, section->parent_bundle);
    WriterWriteChar(w, '/');
    WriterWrite(w, section->promise_type);
}

/**
 * @brief Write a string describing the promise location in policy, e.g. /default/foo/packages/'emacs'
 */
void PromisePath(Writer *w, const Promise *pp)
{
    PromiseTypePath(w, pp->parent_section);
    WriterWriteChar(w, '/');
    WriterWriteChar(w, '\'');
    WriterWrite(w, pp->promiser);
    WriterWriteChar(w, '\'');
}

/**
 * @brief Return handle of the promise.
 * @param pp
 * @return Promise handle or NULL if no handle is provided
 */
const char *PromiseGetHandle(const Promise *pp)
{
    return (const char *)PromiseGetImmediateRvalValue("handle", pp, RVAL_TYPE_SCALAR);
}

/**
 * @brief Get the int value of the first effective constraint found matching, from a promise
 * @param lval
 * @param pp
 * @return Int value, or CF_NOINT
 */
int PromiseGetConstraintAsInt(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    int retval = CF_NOINT;
    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                "Anomalous type mismatch - expected type for int constraint %s did not match internals", lval);
            PromiseRef(LOG_LEVEL_ERR, pp);
            FatalError(ctx, "Aborted");
        }

        retval = (int) IntFromString((char *) cp->rval.item);
    }

    return retval;
}

/*****************************************************************************/

/**
 * @brief Get the real value of the first effective constraint found matching, from a promise
 * @return true if value could be extracted
 */
bool PromiseGetConstraintAsReal(const EvalContext *ctx, const char *lval, const Promise *pp, double *value_out)
{
    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                "Anomalous type mismatch - expected type for int constraint '%s' did not match internals", lval);
            FatalError(ctx, "Aborted");
        }

        *value_out = DoubleFromString((char *) cp->rval.item, value_out);
        return true;
    }

    return false;
}

/*****************************************************************************/

/**
 * @return true if successful
 */
static bool Str2Mode(const char *s, mode_t *mode_out)
{
    int a = CF_UNDEFINED;

    if (s == NULL)
    {
        *mode_out = (mode_t)0;
        return true;
    }

    sscanf(s, "%o", &a);

    if (a == CF_UNDEFINED)
    {
        return false;
    }

    *mode_out = (mode_t)a;
    return true;
}

/**
 * @brief Get the octal value of the first effective constraint found matching, from a promise
 * @param lval
 * @param list
 * @return Double value, or 077 if not found
 */
mode_t PromiseGetConstraintAsOctal(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    mode_t retval = 077;

// We could handle units here, like kb,b,mb

    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                "Anomalous type mismatch - expected type for int constraint %s did not match internals", lval);
            PromiseRef(LOG_LEVEL_ERR, pp);
            FatalError(ctx, "Aborted");
        }

        if (!Str2Mode(cp->rval.item, &retval))
        {
            Log(LOG_LEVEL_ERR, "Error reading assumed octal value '%s'", (const char *)cp->rval.item);
            PromiseRef(LOG_LEVEL_ERR, pp);
        }
    }

    return retval;
}

/*****************************************************************************/

#ifdef __MINGW32__

uid_t PromiseGetConstraintAsUid(const EvalContext *ctx, const char *lval, const Promise *pp)
{                               // we use sids on windows instead
    return CF_SAME_OWNER;
}

#else /* !__MINGW32__ */

/**
 * @brief Get the uid value of the first effective constraint found matching, from a promise
 * @param lval
 * @param pp
 * @return Uid value, or CF_SAME_OWNER if not found
 */
uid_t PromiseGetConstraintAsUid(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    int retval = CF_SAME_OWNER;
    char buffer[CF_MAXVARSIZE];

    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                "Anomalous type mismatch - expected type for owner constraint %s did not match internals",
                lval);
            PromiseRef(LOG_LEVEL_ERR, pp);
            FatalError(ctx, "Aborted");
        }

        retval = Str2Uid((char *) cp->rval.item, buffer, pp);
    }

    return retval;
}

#endif /* !__MINGW32__ */

/*****************************************************************************/

#ifdef __MINGW32__

gid_t PromiseGetConstraintAsGid(const EvalContext *ctx, char *lval, const Promise *pp)
{                               // not applicable on windows: processes have no group
    return CF_SAME_GROUP;
}

#else /* !__MINGW32__ */

/**
 * @brief Get the uid value of the first effective constraint found matching, from a promise
 * @param lval
 * @param pp
 * @return Gid value, or CF_SAME_GROUP if not found
 */
gid_t PromiseGetConstraintAsGid(const EvalContext *ctx, char *lval, const Promise *pp)
{
    int retval = CF_SAME_GROUP;
    char buffer[CF_MAXVARSIZE];

    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_SCALAR)
        {
            Log(LOG_LEVEL_ERR,
                "Anomalous type mismatch - expected type for group constraint '%s' did not match internals",
                lval);
            PromiseRef(LOG_LEVEL_ERR, pp);
            FatalError(ctx, "Aborted");
        }

        retval = Str2Gid((char *) cp->rval.item, buffer, pp);
    }

    return retval;
}
#endif /* !__MINGW32__ */

/*****************************************************************************/

/**
 * @brief Get the Rlist value of the first effective constraint found matching, from a promise
 * @param lval
 * @param list
 * @return Rlist or NULL if not found (note: same as empty list)
 */
// FIX: promise constrained classed?
Rlist *PromiseGetConstraintAsList(const EvalContext *ctx, const char *lval, const Promise *pp)
{
    const Constraint *cp = PromiseGetConstraint(pp, lval);
    if (cp)
    {
        if (cp->rval.type != RVAL_TYPE_LIST)
        {
            Log(LOG_LEVEL_ERR, "Type mismatch on rhs - expected type for list constraint '%s'", lval);
            PromiseRef(LOG_LEVEL_ERR, pp);
            FatalError(ctx, "Aborted");
        }

        return RvalRlistValue(cp->rval);
    }

    return NULL;
}

/**
 * @brief Get the first effective constraint from the promise, also does some checking
 * @param promise
 * @param lval
 * @return Effective constraint if found, otherwise NULL
 */
Constraint *PromiseGetConstraint(const Promise *pp, const char *lval)
{
    if (!pp)
    {
        return NULL;
    }

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp(cp->lval, lval) == 0)
        {
            return cp;
        }
    }

    return NULL;
}

Constraint *PromiseGetConstraintWithType(const Promise *pp, const char *lval, RvalType type)
{
    assert(pp);
    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);
        if (cp->rval.type != type) {
            continue;
        }

        if (strcmp(cp->lval, lval) == 0)
        {
            return cp;
        }
    }

    return NULL;
}

/**
 * @brief Get the first constraint from the promise
 *
 * Kill this function with fire once we have separated promise constraints and body constraints.
 *
 * @param promise
 * @param lval
 * @return Constraint if found, otherwise NULL
 */
Constraint *PromiseGetImmediateConstraint(const Promise *pp, const char *lval)
{
    if (pp == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < SeqLength(pp->conlist); ++i)
    {
        Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp(cp->lval, lval) == 0)
        {
            /* It would be nice to check whether the constraint we have asked
               for is defined in promise (not in referenced body), but there
               seem to be no way to do it easily.

               Checking for absence of classes does not work, as constrains
               obtain classes defined on promise itself.
            */

            return cp;
        }
    }

    return NULL;
}

/**
 * @brief Get the Rval value of the first constraint that matches the given
 * type. Checks that this constraint does not have any contexts attached.
 *
 * Kill this function with fire once we have separated body constraints and bundle constraints.
 *
 * @param lval
 * @param promise
 * @param type
 * @return Rval value if found, NULL otherwise
 */
void *PromiseGetImmediateRvalValue(const char *lval, const Promise *pp, RvalType rtype)
{
    const Constraint *constraint = PromiseGetImmediateConstraint(pp, lval);

    if (constraint && constraint->rval.type == rtype)
    {
        return constraint->rval.item;
    }
    else
    {
        return NULL;
    }
}

/*****************************************************************************/

/**
 * @brief Get the Rval value of the first effective constraint that matches the given type
 * @param lval
 * @param promise
 * @param type
 * @return Rval value if found, NULL otherwise
 */
void *PromiseGetConstraintAsRval(const Promise *pp, const char *lval, RvalType rtype)
{
    const Constraint *constraint = PromiseGetConstraint(pp, lval);

    if (constraint && constraint->rval.type == rtype)
    {
        return constraint->rval.item;
    }
    else
    {
        return NULL;
    }
}

/*****************************************************************************/

/*
 * Check promise constraints while iterating through all slist/containers
 * combinations, called from ExpandPromiseAndDo() during PRE-EVAL.  This is
 * currently running also in cf-promises, but it's enough if such errors are
 * detected in the agent run.
 *
 * TODO remove CommonEvalPromise() actuator, that all it does is call this
 * function, and call this one at the beginning of the main actuators, like
 * KeepAgentPromise().
 */
void PromiseRecheckAllConstraints(const EvalContext *ctx, const Promise *pp)
{
    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);
        SyntaxTypeMatch err = ConstraintCheckType(cp);
        if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
        {
            PolicyError *error =
                PolicyErrorNew(POLICY_ELEMENT_TYPE_CONSTRAINT, cp,
                               "In attribute '%s', %s",
                               cp->lval, SyntaxTypeMatchToString(err));
            char *error_str = PolicyErrorToString(error);

            Log(LOG_LEVEL_ERR, "%s", error_str);
            PolicyErrorDestroy(error);
            free(error_str);

            FatalError(ctx, "Cannot continue");
        }
    }

    /* Check and warn for non-convergence, see commits
       4f8c19b84327b8f3c2e269173196282ccedfdad9 and
       30c109d22e170a781a647b04b4b0a4a2f7244871. */
    if (strcmp(PromiseGetPromiseType(pp), "insert_lines") == 0)
    {
        /* TODO without static var, actually remove this check from here
         * completely, do it at the end of PRE-EVAL promise iterations
         * (ExpandPromiseAndDo() after the main loop). */
        static Item *EDIT_ANCHORS = NULL; /* GLOBAL_X */

        const char *sp = PromiseGetConstraintAsRval(pp, "select_line_matching",
                                                    RVAL_TYPE_SCALAR);
        if (sp != NULL && !IsExpandable(sp))
        {
            /* Avoid adding twice the same select_line_matching anchor. */
            const char *bun = PromiseGetBundle(pp)->name;
            const Item *ptr = ReturnItemInClass(EDIT_ANCHORS, sp, bun);
            if (ptr != NULL)
            {
                Log(LOG_LEVEL_INFO,
                    "insert_lines promise uses the same select_line_matching anchor '%s' as another promise."
                    " This will lead to non-convergent behaviour unless 'empty_file_before_editing' is set",
                    sp);
                PromiseRef(LOG_LEVEL_INFO, pp);
                return;
            }

            PrependItem(&EDIT_ANCHORS, sp, bun);
        }
    }
}

/*****************************************************************************/

static SyntaxTypeMatch ConstraintCheckType(const Constraint *cp)
{
    // Check class
    for (size_t i = 0; CF_CLASSBODY[i].lval != NULL; i++)
    {
        if (strcmp(cp->lval, CF_CLASSBODY[i].lval) == 0)
        {
            SyntaxTypeMatch err = CheckConstraintTypeMatch(cp->lval, cp->rval, CF_CLASSBODY[i].dtype, CF_CLASSBODY[i].range.validation_string, 0);
            if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
            {
                return err;
            }
        }
    }

    if (cp->type == POLICY_ELEMENT_TYPE_PROMISE)
    {
        BundleSection *section = cp->parent.promise->parent_section;

        for (size_t i = 0; i < (size_t) CF3_MODULES; i++)
        {
            const PromiseTypeSyntax *ssp = CF_ALL_PROMISE_TYPES[i];
            if (!ssp)
            {
                continue;
            }

            for (size_t j = 0; ssp[j].bundle_type != NULL; j++)
            {
                PromiseTypeSyntax ss = ssp[j];

                if (ss.promise_type != NULL)
                {
                    if (strcmp(ss.promise_type, section->promise_type) == 0)
                    {
                        const ConstraintSyntax *bs = ss.constraints;

                        for (size_t l = 0; bs[l].lval != NULL; l++)
                        {
                            // No validation for CF_DATA_TYPE_BUNDLE here
                            // see: PolicyCheckUndefinedBundles() etc.
                            if (bs[l].dtype == CF_DATA_TYPE_BODY)
                            {
                                const ConstraintSyntax *bs2 = bs[l].range.body_type_syntax->constraints;

                                for (size_t m = 0; bs2[m].lval != NULL; m++)
                                {
                                    if (strcmp(cp->lval, bs2[m].lval) == 0)
                                    {
                                        return CheckConstraintTypeMatch(cp->lval, cp->rval, bs2[m].dtype, bs2[m].range.validation_string, 0);
                                    }
                                }
                            }

                            if (strcmp(cp->lval, bs[l].lval) == 0)
                            {
                                return CheckConstraintTypeMatch(cp->lval, cp->rval, bs[l].dtype, bs[l].range.validation_string, 0);
                            }
                        }
                    }
                }
            }
        }
    }

/* Now check the functional modules - extra level of indirection */

    for (size_t i = 0; CF_COMMON_BODIES[i].lval != NULL; i++)
    {
        if (CF_COMMON_BODIES[i].dtype == CF_DATA_TYPE_BODY)
        {
            continue;
        }

        if (strcmp(cp->lval, CF_COMMON_BODIES[i].lval) == 0)
        {
            return CheckConstraintTypeMatch(cp->lval, cp->rval, CF_COMMON_BODIES[i].dtype, CF_COMMON_BODIES[i].range.validation_string, 0);
        }
    }

    return SYNTAX_TYPE_MATCH_OK;
}

/**
 * @brief Check whether the promise type is allowed one
 */
/* FIXME: need to be done automatically */
bool BundleTypeCheck(const char *name)
{
    /* FIXME: export size of CF_AGENTTYPES somewhere */
    for (int i = 0; strcmp(CF_AGENTTYPES[i], "<notype>") != 0; ++i)
    {
        if (!strcmp(CF_AGENTTYPES[i], name))
        {
            return true;
        }
    }

    if (!strcmp("knowledge", name))
    {
        return true;
    }

    if (!strcmp("edit_line", name))
    {
        return true;
    }

    if (!strcmp("edit_xml", name))
    {
        return true;
    }

    return false;
}

bool PolicyHasCustomPromiseType(const Policy *policy, const char *name)
{
    assert(policy != NULL);
    Seq *types = policy->custom_promise_types;
    const size_t length = SeqLength(types);
    for (size_t i = 0; i < length; ++i)
    {
        Body *type = SeqAt(types, i);
        if (StringEqual(name, type->name))
        {
            return true;
        }
    }
    return false;
}
