/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2006 Stephen Deasey <sdeasey@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * nsdbimy.c --
 *
 *      Implements the DBI database driver callbacks for MySQL.
 */

#include "nsdbidrv.h"
#include <mysql/mysql.h>
#include <mysql/errmsg.h>


NS_EXPORT int Ns_ModuleVersion = 1;


/*
 * The following sructure manages per-pool configuration.
 */

typedef struct MyConfig {
    char        *module;
    CONST char  *db;
    CONST char  *user;
    CONST char  *password;
    CONST char  *host;
    int          port;
    CONST char  *unixdomain;
} MyConfig;


/*
 * The following structure tracks a single connection to the
 * database and the current result set.
 */

typedef struct MyHandle {

    MyConfig      *myCfg;    /* Config values for handles in a pool. */
    MYSQL         *conn;     /* Connection to a MySQL database. */

    Dbi_Isolation  defaultIsolation;

    MYSQL_BIND     bind[64]; /* Scratch output bind buffers for result rows. */

} MyHandle;

/*
 * The following structure manages a prepared statement
 * and it's input bind buffers.
 */

typedef struct MyStatement {

    MYSQL_STMT    *st;       /* A MySQL statement. */
    MYSQL_RES     *meta;     /* Result set describing column data. */
    Ns_DString     valueDs;  /* Buffer to hold a single result value. */

} MyStatement;


/*
 * Static functions defined in this file.
 */

static Dbi_OpenProc         Open;
static Dbi_CloseProc        Close;
static Dbi_ConnectedProc    Connected;
static Dbi_BindVarProc      Bind;
static Dbi_PrepareProc      Prepare;
static Dbi_PrepareCloseProc PrepareClose;
static Dbi_ExecProc         Exec;
static Dbi_NextValueProc    NextValue;
static Dbi_ColumnNameProc   ColumnName;
static Dbi_TransactionProc  Transaction;
static Dbi_FlushProc        Flush;
static Dbi_ResetProc        Reset;

static int IsolationLevel(Dbi_Handle *handle, Dbi_Isolation isolation);
static void MyException(Dbi_Handle *, MYSQL_STMT *);

static void InitThread(void);
static Ns_TlsCleanup CleanupThread;


/*
 * Static variables defined in this file.
 */

static Dbi_DriverProc procs[] = {
    {Dbi_OpenProcId,         Open},
    {Dbi_CloseProcId,        Close},
    {Dbi_ConnectedProcId,    Connected},
    {Dbi_BindVarProcId,      Bind},
    {Dbi_PrepareProcId,      Prepare},
    {Dbi_PrepareCloseProcId, PrepareClose},
    {Dbi_ExecProcId,         Exec},
    {Dbi_NextValueProcId,    NextValue},
    {Dbi_ColumnNameProcId,   ColumnName},
    {Dbi_TransactionProcId,  Transaction},
    {Dbi_FlushProcId,        Flush},
    {Dbi_ResetProcId,        Reset},
    {0, NULL}
};

static Ns_Tls tls; /* For the thread exit callback. */



/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Register the driver functions.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT int
Ns_ModuleInit(CONST char *server, CONST char *module)
{
    MyConfig          *myCfg;
    char              *path;
    static CONST char *drivername = "my";
    static CONST char *database   = "mysql";
    static int         once = 0;

    if (!mysql_thread_safe()) {
        Ns_Log(Error, "dbimy: mysql library not compiled thread safe");
        return NS_ERROR;
    }

    if (!once) {
        once = 1;
        Ns_TlsAlloc(&tls, CleanupThread);
        if (mysql_server_init(0, NULL, NULL)) {
            return NS_ERROR;
        }
    }

    path = Ns_ConfigGetPath(server, module, NULL);

    myCfg = ns_malloc(sizeof(MyConfig));
    myCfg->module     = ns_strdup(module);
    myCfg->db         = Ns_ConfigString(path, "database",   "mysql");
    myCfg->user       = Ns_ConfigString(path, "user",       "root");
    myCfg->password   = Ns_ConfigString(path, "password",   NULL);
    myCfg->host       = Ns_ConfigString(path, "host",       NULL);
    myCfg->port       = Ns_ConfigInt(path,    "port",       0);
    myCfg->unixdomain = Ns_ConfigString(path, "unixdomain", NULL);

    return Dbi_RegisterDriver(server, module,
                              drivername, database,
                              procs, myCfg);
}


/*
 *----------------------------------------------------------------------
 *
 * Open --
 *
 *      Open a connection to the configured mysql database.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Open(ClientData configData, Dbi_Handle *handle)
{
    MyConfig *myCfg = configData;
    MyHandle *myHandle;
    MYSQL    *conn;

    InitThread();

    conn = mysql_init(NULL);
    mysql_options(conn, MYSQL_READ_DEFAULT_GROUP, "nsdbimysql");
    if (!mysql_real_connect(conn, myCfg->host, myCfg->user, myCfg->password,
                            myCfg->db, myCfg->port, myCfg->unixdomain, 0)) {

        Dbi_SetException(handle, "dbimysql", "code: %d msg: %s",
                         mysql_errno(conn), mysql_error(conn));
        mysql_close(conn);
        return NS_ERROR;
    }
    myHandle = ns_calloc(1, sizeof(MyHandle));
    myHandle->conn = conn;
    handle->driverData = myHandle;

    /*
     * Make sure the database is expecting and returning utf8 character data.
     */

    if (Dbi_ExecDirect(handle, "set names 'utf8'") != NS_OK) {
        return NS_ERROR;
    }

    /*
     * Extra handle info to help with debuging.
     */

    Dbi_SetException(handle, "00000", "(%s)",
                     mysql_get_server_info(conn));

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
 *
 *      Close a database connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
Close(Dbi_Handle *handle)
{
    MyHandle *myHandle = handle->driverData;

    assert(myHandle);

    mysql_close(myHandle->conn);
    ns_free(myHandle);

    handle->driverData = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Connected --
 *
 *      Is the given handle currently connected?
 *
 * Results:
 *      NS_TRUE if connected, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Connected(Dbi_Handle *handle)
{
    MyHandle *myHandle = handle->driverData;

    if (myHandle != NULL
            && myHandle->conn != NULL
            && !mysql_ping(myHandle->conn)) {
        return NS_TRUE;
    }
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Bind --
 *
 *      Append a bind variable place holder in MySQL syntax to the
 *      given dstring. MySQL uses ? as a place holder.
 *
 * Results:
 *      Always NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
Bind(Ns_DString *ds, CONST char *name, int bindIdx)
{
    Ns_DStringAppend(ds, "?");
}


/*
 *----------------------------------------------------------------------
 *
 * Prepare --
 *
 *      Prepare a statement if one doesn't already exist for this query.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Prepare(Dbi_Handle *handle, Dbi_Statement *stmt,
        unsigned int *numVarsPtr, unsigned int *numColsPtr)
{
    MyHandle      *myHandle = handle->driverData;
    MyStatement   *myStmt;
    MYSQL_STMT    *st;
    MYSQL_RES     *meta;

    InitThread();

    if (stmt->driverData == NULL) {

        if ((st = mysql_stmt_init(myHandle->conn)) == NULL) {
            Ns_Fatal("dbimysql: Prepare: out of memory allocating statement.");
        }
        if (mysql_stmt_prepare(st, stmt->sql, stmt->length)) {
            MyException(handle, st);
            mysql_stmt_close(st);
            return NS_ERROR;
        }
        *numVarsPtr = mysql_stmt_param_count(st);
        *numColsPtr = mysql_stmt_field_count(st);

        /*
         * Save the column metadata so we can figure out whether
         * to ask for binary or text values when fetching.
         */

        if (*numColsPtr > 0) {
            meta = mysql_stmt_result_metadata(st);
            if (meta == NULL) {
                MyException(handle, st);
                (void) mysql_stmt_close(st);
                return NS_ERROR;
            }
        } else {
            meta = NULL;
        }

        myStmt = ns_malloc(sizeof(MyStatement));
        myStmt->st = st;
        myStmt->meta = meta;
        Ns_DStringInit(&myStmt->valueDs);

        stmt->driverData = myStmt;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PrepareClose --
 *
 *      Cleanup a prepared statement.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
PrepareClose(Dbi_Handle *handle, Dbi_Statement *stmt)
{
    MyStatement *myStmt = stmt->driverData;

    assert(myStmt);

    if (myStmt->meta != NULL) {
        mysql_free_result(myStmt->meta);
    }
    mysql_stmt_close(myStmt->st);
    Ns_DStringFree(&myStmt->valueDs);
    ns_free(myStmt);

    stmt->driverData = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Exec --
 *
 *      Bind values and execute the statement.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      ...
 *
 *----------------------------------------------------------------------
 */

static int
Exec(Dbi_Handle *handle, Dbi_Statement *stmt,
     Dbi_Value *values, unsigned int numValues)
{
    MyHandle    *myHandle = handle->driverData;
    MyStatement *myStmt   = stmt->driverData;
    MYSQL_BIND   bind[DBI_MAX_BIND];
    int          i;

    InitThread();

    /*
     * Bind values to parameters.
     */

    if (numValues > 0) {

        memset(bind, 0, sizeof(bind));

        for (i = 0; i < numValues; i++) {
            if (values[i].data != NULL) {
                bind[i].buffer_type = values[i].binary
                    ? MYSQL_TYPE_BLOB
                    : MYSQL_TYPE_STRING;
            } else {
                bind[i].buffer_type = MYSQL_TYPE_NULL;
            }
            bind[i].buffer        = (char *) values[i].data;
            bind[i].buffer_length = values[i].length;
        }

        if (mysql_stmt_bind_param(myStmt->st, bind)) {
            MyException(handle, myStmt->st);
            return NS_ERROR;
        }
    }

    /*
     * Execute the statment and buffer the entire result set.
     */

    if (mysql_stmt_execute(myStmt->st)) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }
    /* buffer entire result: mysql_stmt_store_result(myStmt->st) */

    if (mysql_stmt_field_count(myStmt->st)
            && mysql_stmt_bind_result(myStmt->st, myHandle->bind)) {
        MyException(handle, myStmt->st);
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NextValue --
 *
 *      Fetch the value of the given row and column.
 *
 * Results:
 *      DBI_VALUE, DBI_DONE, DBI_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NextValue(Dbi_Handle *handle, Dbi_Statement *stmt,
          unsigned int colIdx, unsigned int rowIdx, Dbi_Value *value)
{
    MyStatement           *myStmt = stmt->driverData;
    Ns_DString            *ds     = &myStmt->valueDs;
    MYSQL_BIND             bind;
    MYSQL_FIELD           *field;
    enum enum_field_types  buffer_type;
    my_bool                is_null, error;
    unsigned long          length;

    /*
     * Reposition to the next row.
     */

    if (colIdx == 0) {

        switch (mysql_stmt_fetch(myStmt->st)) {

        case MYSQL_NO_DATA:
            return DBI_DONE;

        case 1:
            return DBI_ERROR;

        case 0:
        case MYSQL_DATA_TRUNCATED:
            /* fallthrough */
            break;
        }
    }

    /*
     * Check the actual column type of the result so we can ask
     * for binary blobs in native format. Everything else we ask
     * mysql to convert to a string.
     */

    field = mysql_fetch_field_direct(myStmt->meta, colIdx);
    if (field == NULL) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }

    switch (field->type) {

    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
        buffer_type = MYSQL_TYPE_BLOB;
        break;

    default:
        buffer_type = MYSQL_TYPE_STRING;
    }

    /*
     * Attempt to fetch the value into the existing space of a
     * dstring buffer.  If that fails, resize the dstring.
     */

    Ns_DStringSetLength(ds, MAX(ds->spaceAvl, TCL_DSTRING_STATIC_SIZE) -1);

    memset(&bind, 0, sizeof(bind));
    bind.buffer        = Ns_DStringValue(ds);
    bind.buffer_length = Ns_DStringLength(ds);
    bind.length        = &length;
    bind.is_null       = &is_null;
    bind.error         = &error;
    bind.buffer_type   = buffer_type;

    length = 0;
    is_null = 0;
    error = 0;

    if (mysql_stmt_fetch_column(myStmt->st, &bind, colIdx, 0)) {
        MyException(handle, myStmt->st);
        return DBI_ERROR;
    }

    if (length > bind.buffer_length) {

        Ns_DStringSetLength(ds, (int) length);
        bind.buffer        = Ns_DStringValue(ds);
        bind.buffer_length = Ns_DStringLength(ds);

        if (mysql_stmt_fetch_column(myStmt->st, &bind, colIdx, 0)) {
            MyException(handle, myStmt->st);
            return DBI_ERROR;
        }
    } else {
        Ns_DStringSetLength(ds, (int) length);
    }

    /*
     * Return the value.
     */

    value->data   = is_null ? NULL : Ns_DStringValue(ds);
    value->length = (unsigned int) Ns_DStringLength(ds);
    value->binary = (buffer_type == MYSQL_TYPE_BLOB) ? 1 : 0;

    return DBI_VALUE;
}


/*
 *----------------------------------------------------------------------
 *
 * ColumnName --
 *
 *      Fetch the UTF8 column name for the current statement.
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ColumnName(Dbi_Handle *handle, Dbi_Statement *stmt,
           unsigned int index, CONST char **columnPtr)
{
    MyStatement *myStmt = stmt->driverData;
    MYSQL_FIELD *field;

    field = mysql_fetch_field_direct(myStmt->meta, index);
    if (field == NULL) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }

    /*
     * NB: The memory for 'name' is stored within the column meta
     *     data result structure so it is OK to return a pointer
     *     to it here.
     */

    *columnPtr = field->name;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Transaction --
 *
 *      Begin, commit and rollback transactions.
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Transaction(Dbi_Handle *handle, unsigned int depth,
            Dbi_TransactionCmd cmd, Dbi_Isolation isolation)
{
    MyHandle   *myHandle = handle->driverData;
    Ns_DString  ds;

    switch (cmd) {

    case Dbi_TransactionBegin:
        if (depth == 0) {
            if (IsolationLevel(handle, isolation) != NS_OK
                    || mysql_query(myHandle->conn, "start transaction")) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                return NS_ERROR;
            }
        } else {
            Ns_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "savepoint s%u", depth);

            if (mysql_query(myHandle->conn, Ns_DStringValue(&ds))) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                Ns_DStringFree(&ds);
                return NS_ERROR;
            }
            Ns_DStringFree(&ds);
        }
        break;

    case Dbi_TransactionCommit:
        if (mysql_commit(myHandle->conn)
                || IsolationLevel(handle, isolation) != NS_OK) {
            Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                             mysql_error(myHandle->conn));
            return NS_ERROR;
        }
        break;

    case Dbi_TransactionRollback:
        if (depth == 0) {
            if (mysql_rollback(myHandle->conn)
                    || IsolationLevel(handle, isolation)) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                return NS_ERROR;
            }
        } else {
            Ns_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "rollback to savepoint s%u", depth);
            if (mysql_query(myHandle->conn, Ns_DStringValue(&ds))) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                Ns_DStringFree(&ds);
                return NS_ERROR;
            }
            Ns_DStringFree(&ds);
        }
        break;
    }

    return NS_OK;
}

static int
IsolationLevel(Dbi_Handle *handle, Dbi_Isolation isolation)
{
    MyHandle *myHandle = handle->driverData;

    static CONST char *levels[] = {
        "set transaction isolation level read uncommitted", /* Dbi_ReadUncommitted */
        "set transaction isolation level read committed",   /* Dbi_ReadCommitted */
        "set transaction isolation level repeatable read",  /* Dbi_RepeatableRead */
        "set transaction isolation level serializable"      /* Dbi_Serializable */
    };

    if (isolation != myHandle->defaultIsolation
            && mysql_query(myHandle->conn, levels[isolation])) {
        return NS_ERROR;
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Flush --
 *
 *      Clear the current result, which discards any pending rows.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Flush(Dbi_Handle *handle, Dbi_Statement *stmt)
{
    MyStatement *myStmt = stmt->driverData;
    int          status = NS_OK;

    assert(myStmt);

    if (myStmt->st && mysql_stmt_reset(myStmt->st)) {
        MyException(handle, myStmt->st);
        status = NS_ERROR;
    }
    Ns_DStringFree(&myStmt->valueDs);
    Ns_DStringInit(&myStmt->valueDs);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Reset --
 *
 *      Reset the handle...
 *
 * Results:
 *      Always NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Reset(Dbi_Handle *handle)
{
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MyException --
 *
 *      Report a MySQL exception to the dbi layer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Fatal exit if memory exhausted.
 *
 *----------------------------------------------------------------------
 */

static void
MyException(Dbi_Handle *handle, MYSQL_STMT *st)
{
    if (mysql_stmt_errno(st) == CR_OUT_OF_MEMORY) {
        Ns_Fatal("dbimysql: CR_OUT_OF_MEMORY: %s",
                 mysql_stmt_error(st));
    }
    Dbi_SetException(handle, mysql_stmt_sqlstate(st),
                     mysql_stmt_error(st));
}


/*
 *----------------------------------------------------------------------
 *
 * InitThread, CleanupThread --
 *
 *      Initialise and cleanup msql thread data for each thread
 *      that calls a mysql_* function.
 *
 *      InitThread is called from Open and Exec, the two functions
 *      which a thread can call before calling any other dbi function.
 *
 *      CleanupThread is a Tls callback which gets called only when a
 *      thread exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      MySQL memory is freed.
 *
 *----------------------------------------------------------------------
 */

static void
InitThread(void)
{
    int initialised;

    initialised = (int) Ns_TlsGet(&tls);
    if (!initialised) {
        Ns_TlsSet(&tls, (void *) NS_TRUE);
        Ns_Log(Debug, "dbimy: InitThread");
        mysql_thread_init();
    }
}

static void
CleanupThread(void *arg)
{
    int initialised = (int) arg;

    if (initialised) {
        Ns_Log(Debug, "dbimy: CleanupThread");
        mysql_thread_end();
    }
}
