/* -------------------------------------------------------------------------
 *
 *	common.c
 *		Common support routines for bin/scripts/
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/common.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include "common.h"
#include "libpq/pqsignal.h"

static void SetCancelConn(PGconn* conn);
static void ResetCancelConn(void);

static PGcancel* volatile cancelConn = NULL;

#ifdef WIN32
static CRITICAL_SECTION cancelConnLock;
#endif

/*
 * Returns the current user name.
 */
const char* get_user_name(const char* progname)
{
#ifndef WIN32
    struct passwd* pw;

    pw = getpwuid(geteuid());
    if (pw == NULL) {
        fprintf(stderr, _("%s: could not obtain information about current user: %s\n"), progname, strerror(errno));
        exit(1);
    }
    return pw->pw_name;
#else
    static char username[128]; /* remains after function exit */
    DWORD len = sizeof(username) - 1;

    if (!GetUserName(username, &len)) {
        fprintf(stderr, _("%s: could not get current user name: %s\n"), progname, strerror(errno));
        exit(1);
    }
    return username;
#endif
}

/*
 * Provide strictly harmonized handling of --help and --version
 * options.
 */
void handle_help_version_opts(int argc, char* argv[], const char* fixed_progname, help_handler hlp)
{
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0) {
            const char* progname_tmp = get_progname(argv[0]);
            hlp(progname_tmp);
            free(const_cast<char*>(progname_tmp));
            exit(0);
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            printf("%s (PostgreSQL) " PG_VERSION "\n", fixed_progname);
            exit(0);
        }
    }
}

/*
 * Make a database connection with the given parameters.  An
 * interactive password prompt is automatically issued if required.
 */
PGconn* connectDatabase(const char* dbname, const char* pghost, const char* pgport, const char* pguser,
    enum trivalue prompt_password, const char* progname, bool fail_ok)
{
    PGconn* conn = NULL;
    char* password = NULL;
    bool new_pass = false;

    if (prompt_password == TRI_YES)
        password = simple_prompt("Password: ", 100, false);

    /*
     * Start the connection.  Loop until we have a password if requested by
     * backend.
     */
    do {
#define PARAMS_ARRAY_SIZE 7
        const char** keywords = (const char**)malloc(PARAMS_ARRAY_SIZE * sizeof(*keywords));
        const char** values = (const char**)malloc(PARAMS_ARRAY_SIZE * sizeof(*values));

        if ((keywords == NULL) || (values == NULL)) {
            fprintf(stderr, _("%s: out of memory\n"), progname);
            exit(1);
        }

        keywords[0] = "host";
        values[0] = pghost;
        keywords[1] = "port";
        values[1] = pgport;
        keywords[2] = "user";
        values[2] = pguser;
        keywords[3] = "password";
        values[3] = password;
        keywords[4] = "dbname";
        values[4] = dbname;
        keywords[5] = "fallback_application_name";
        values[5] = progname;
        keywords[6] = NULL;
        values[6] = NULL;

        new_pass = false;
        conn = PQconnectdbParams(keywords, values, true);

        free(keywords);
        free(values);

        if (conn == NULL) {
            fprintf(stderr, _("%s: could not connect to database %s\n"), progname, dbname);
            exit(1);
        }

        if (PQstatus(conn) == CONNECTION_BAD && PQconnectionNeedsPassword(conn) && password == NULL &&
            prompt_password != TRI_NO) {
            PQfinish(conn);
            password = simple_prompt("Password: ", 100, false);
            new_pass = true;
        }
    } while (new_pass);

    if (password != NULL) {
        errno_t ret = memset_s(password, strlen(password), 0, strlen(password));
        check_memset_s(ret);
        free(password);
    }

    /* check to see that the backend connection was successfully made */
    if (PQstatus(conn) == CONNECTION_BAD) {
        if (fail_ok) {
            PQfinish(conn);
            return NULL;
        }
        fprintf(stderr, _("%s: could not connect to database %s: %s"), progname, dbname, PQerrorMessage(conn));
        exit(1);
    }

    return conn;
}

/*
 * Try to connect to the appropriate maintenance database.
 */
PGconn* connectMaintenanceDatabase(const char* maintenance_db, const char* pghost, const char* pgport,
    const char* pguser, enum trivalue prompt_password, const char* progname)
{
    PGconn* conn = NULL;

    /* If a maintenance database name was specified, just connect to it. */
    if (maintenance_db != NULL)
        return connectDatabase(maintenance_db, pghost, pgport, pguser, prompt_password, progname, false);

    /* Otherwise, try postgres first and then template1. */
    conn = connectDatabase("postgres", pghost, pgport, pguser, prompt_password, progname, true);
    if (conn == NULL)
        conn = connectDatabase("template1", pghost, pgport, pguser, prompt_password, progname, false);

    return conn;
}

/*
 * Run a query, return the results, exit program on failure.
 */
PGresult* executeQuery(PGconn* conn, const char* query, const char* progname, bool echo)
{
    PGresult* res = NULL;

    if (echo)
        printf("%s\n", query);

    res = PQexec(conn, query);
    if ((res == NULL) || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, _("%s: query failed: %s"), progname, PQerrorMessage(conn));
        fprintf(stderr, _("%s: query was: %s\n"), progname, query);
        PQfinish(conn);
        exit(1);
    }

    return res;
}

/*
 * As above for a SQL command (which returns nothing).
 */
void executeCommand(PGconn* conn, const char* query, const char* progname, bool echo)
{
    PGresult* res = NULL;

    if (echo)
        printf("%s\n", query);

    res = PQexec(conn, query);
    if ((res == NULL) || PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, _("%s: query failed: %s"), progname, PQerrorMessage(conn));
        fprintf(stderr, _("%s: query was: %s\n"), progname, query);
        PQfinish(conn);
        exit(1);
    }

    PQclear(res);
}

/*
 * As above for a SQL maintenance command (returns command success).
 * Command is executed with a cancel handler set, so Ctrl-C can
 * interrupt it.
 */
bool executeMaintenanceCommand(PGconn* conn, const char* query, bool echo)
{
    PGresult* res = NULL;
    bool r = false;

    if (echo)
        printf("%s\n", query);

    SetCancelConn(conn);
    res = PQexec(conn, query);
    ResetCancelConn();

    r = ((res != NULL) && PQresultStatus(res) == PGRES_COMMAND_OK);

    if (res != NULL)
        PQclear(res);

    return r;
}

/*
 * "Safe" wrapper around strdup().	Pulled from psql/common.c
 */
char* pg_strdup(const char* string)
{
    char* tmp = NULL;

    if (string == NULL) {
        fprintf(stderr, _("pg_strdup: cannot duplicate null pointer (internal error)\n"));
        exit(EXIT_FAILURE);
    }
    tmp = strdup(string);
    if (tmp == NULL) {
        fprintf(stderr, _("out of memory\n"));
        exit(EXIT_FAILURE);
    }
    return tmp;
}

/*
 * Check yes/no answer in a localized way.	1=yes, 0=no, -1=neither.
 */
/* translator: abbreviation for "yes" */
#define PG_YESLETTER gettext_noop("y")
/* translator: abbreviation for "no" */
#define PG_NOLETTER gettext_noop("n")

bool yesno_prompt(const char* question)
{
    char prompt[256];
    int ret;

    /* ------
       translator: This is a question followed by the translated options for
       "yes" and "no". */
    ret = snprintf_s(
        prompt, sizeof(prompt), sizeof(prompt) - 1, _("%s (%s/%s) "), _(question), _(PG_YESLETTER), _(PG_NOLETTER));
    securec_check_ss_c(ret, "\0", "\0");

    for (;;) {
        char* resp = NULL;

        resp = simple_prompt(prompt, 1, true);
        if (strcmp(resp, _(PG_YESLETTER)) == 0) {
            free(resp);
            return true;
        } else if (strcmp(resp, _(PG_NOLETTER)) == 0) {
            free(resp);
            return false;
        }

        free(resp);
        printf(_("Please answer \"%s\" or \"%s\".\n"), _(PG_YESLETTER), _(PG_NOLETTER));
    }
}

/*
 * SetCancelConn
 *
 * Set cancelConn to point to the current database connection.
 */
static void SetCancelConn(PGconn* conn)
{
    PGcancel* oldCancelConn = NULL;

#ifdef WIN32
    EnterCriticalSection(&cancelConnLock);
#endif

    /* Free the old one if we have one */
    oldCancelConn = cancelConn;

    /* be sure handle_sigint doesn't use pointer while freeing */
    cancelConn = NULL;

    if (oldCancelConn != NULL)
        PQfreeCancel(oldCancelConn);

    cancelConn = PQgetCancel(conn);

#ifdef WIN32
    LeaveCriticalSection(&cancelConnLock);
#endif
}

/*
 * ResetCancelConn
 *
 * Free the current cancel connection, if any, and set to NULL.
 */
static void ResetCancelConn(void)
{
    PGcancel* oldCancelConn = NULL;

#ifdef WIN32
    EnterCriticalSection(&cancelConnLock);
#endif

    oldCancelConn = cancelConn;

    /* be sure handle_sigint doesn't use pointer while freeing */
    cancelConn = NULL;

    if (oldCancelConn != NULL)
        PQfreeCancel(oldCancelConn);

#ifdef WIN32
    LeaveCriticalSection(&cancelConnLock);
#endif
}

#ifndef WIN32
/*
 * Handle interrupt signals by canceling the current command,
 * if it's being executed through executeMaintenanceCommand(),
 * and thus has a cancelConn set.
 */
static void handle_sigint(SIGNAL_ARGS)
{
    int save_errno = errno;
    char errbuf[256];

    /* Send QueryCancel if we are processing a database query */
    if (cancelConn != NULL) {
        if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
            fprintf(stderr, _("Cancel request sent\n"));
        else
            fprintf(stderr, _("Could not send cancel request: %s"), errbuf);
    }

    errno = save_errno; /* just in case the write changed it */
}

void setup_cancel_handler(void)
{
    pqsignal(SIGINT, handle_sigint);
}
#else /* WIN32 */

/*
 * Console control handler for Win32. Note that the control handler will
 * execute on a *different thread* than the main one, so we need to do
 * proper locking around those structures.
 */
static BOOL WINAPI consoleHandler(DWORD dwCtrlType)
{
    char errbuf[256];

    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        /* Send QueryCancel if we are processing a database query */
        EnterCriticalSection(&cancelConnLock);
        if (cancelConn != NULL) {
            if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
                fprintf(stderr, _("Cancel request sent\n"));
            else
                fprintf(stderr, _("Could not send cancel request: %s"), errbuf);
        }
        LeaveCriticalSection(&cancelConnLock);

        return TRUE;
    } else
        /* Return FALSE for any signals not being handled */
        return FALSE;
}

void setup_cancel_handler(void)
{
    InitializeCriticalSection(&cancelConnLock);

    SetConsoleCtrlHandler(consoleHandler, TRUE);
}

#endif /* WIN32 */

/*
 * GetEnvStr
 *
 * Note: malloc space for get the return of getenv() function, then return the malloc space.
 *         so, this space need be free.
 */
char* GetEnvStr(const char* env)
{
    char* tmpvar = NULL;
    const char* temp = getenv(env);
    errno_t rc = 0;
    if (temp != NULL) {
        size_t len = strlen(temp);
        if (len == 0) {
            return NULL;
        }
        tmpvar = (char*)malloc(len + 1);
        if (tmpvar != NULL) {
            rc = strcpy_s(tmpvar, len + 1, temp);
            securec_check_c(rc, "\0", "\0");
            return tmpvar;
        }
    }
    return NULL;
}
