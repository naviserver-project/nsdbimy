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
    int          embed;
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

    MYSQL_BIND     bind[DBI_MAX_BIND];
    unsigned long  lengths[DBI_MAX_BIND];
    my_bool        nulls[DBI_MAX_BIND];

} MyHandle;

/*
 * The following structure manages a prepared statement.
 */

typedef struct MyStatement {

    MYSQL_STMT    *st;       /* A MySQL statement. */
    MYSQL_RES     *meta;     /* Result set describing column data. */

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
static Dbi_NextRowProc      NextRow;
static Dbi_ColumnLengthProc ColumnLength;
static Dbi_ColumnValueProc  ColumnValue;
static Dbi_ColumnNameProc   ColumnName;
static Dbi_TransactionProc  Transaction;
static Dbi_FlushProc        Flush;
static Dbi_ResetProc        Reset;

static int IsolationLevel(Dbi_Handle *handle, Dbi_Isolation isolation);
static void MyException(Dbi_Handle *, MYSQL_STMT *);

static void InitThread(void);
static Ns_TlsCleanup CleanupThread;
static Ns_Callback AtExit;


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
    {Dbi_NextRowProcId,      NextRow},
    {Dbi_ColumnLengthProcId, ColumnLength},
    {Dbi_ColumnValueProcId,  ColumnValue},
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
    static CONST char *drivername = "dbimy";
    static CONST char *database   = "mysql";
    static int         once = 0;

    Dbi_LibInit();

    if (!mysql_thread_safe()) {
        Ns_Log(Error, "dbimy: mysql library not compiled thread safe");
        return NS_ERROR;
    }

    if (!once) {
        once = 1;
        if (mysql_library_init(0, NULL, NULL)) {
            return NS_ERROR;
        }
        Ns_TlsAlloc(&tls, CleanupThread);
        Ns_RegisterAtExit(AtExit, NULL);
        Ns_RegisterProcInfo(AtExit, "dbimy:cleanshutdown", NULL);
    }

    path = Ns_ConfigGetPath(server, module, NULL);

    myCfg = ns_malloc(sizeof(MyConfig));
    myCfg->module     = ns_strdup(module);
    myCfg->embed      = Ns_ConfigBool(path,   "embed",      0);
    myCfg->db         = Ns_ConfigString(path, "database",   "mysql");
    myCfg->user       = Ns_ConfigString(path, "user",       "root");
    myCfg->password   = Ns_ConfigString(path, "password",   NULL);
    myCfg->host       = Ns_ConfigString(path, "host",       NULL);
    myCfg->port       = Ns_ConfigInt(path,    "port",       0);
    myCfg->unixdomain = Ns_ConfigString(path, "unixdomain", NULL);

    if (*myCfg->db == '\0') {
        Ns_Log(Error, "dbimy[%s]: database '' is invalid", module);
        return NS_ERROR;
    }

    if (myCfg->embed && !mysql_embedded()) {
        Ns_Log(Error, "dbimy[%s]: driver not compiled with embedded capability",
               module);
        return NS_ERROR;
    }

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
    int       i;

    InitThread();

    conn = mysql_init(NULL);
    if (!conn) {
        Ns_Fatal("dbimy: Open: mysql_init() failed");
    }

    if (myCfg->embed) {
        mysql_options(conn, MYSQL_OPT_USE_EMBEDDED_CONNECTION, NULL);
    } else {
        mysql_options(conn, MYSQL_OPT_USE_REMOTE_CONNECTION, NULL);
    }

    /*
     * Set the group within the my.cnf file to read dbi options from.
     */

    mysql_options(conn, MYSQL_READ_DEFAULT_FILE, "./dbimy.cnf");
    mysql_options(conn, MYSQL_READ_DEFAULT_GROUP, "dbimy");

    /*
     * Connect and make sure we're in autocomit mode.
     */

    if (!mysql_real_connect(conn, myCfg->host, myCfg->user, myCfg->password,
                            myCfg->db, myCfg->port, myCfg->unixdomain, 0)
        || mysql_autocommit(conn, 1)) {

        Dbi_SetException(handle, mysql_sqlstate(conn), mysql_error(conn));
        mysql_close(conn);
        return NS_ERROR;
    }

    myHandle = ns_calloc(1, sizeof(MyHandle));
    myHandle->conn = conn;
    handle->driverData = myHandle;

    for (i = 0; i < DBI_MAX_BIND; i++) {
        myHandle->bind[i].length = myHandle->lengths + i;
        myHandle->bind[i].is_null = myHandle->nulls + i;
        myHandle->bind[i].buffer_type = MYSQL_TYPE_STRING;
    }

    /*
     * Make sure the database is expecting and returning utf8 character data.
     * Refuse to load if this doesn't work.
     */

    if (Dbi_ExecDirect(handle, "set names 'utf8'") != NS_OK) {
        Dbi_LogException(handle, Error);

        mysql_close(myHandle->conn);
        ns_free(myHandle);
        handle->driverData = NULL;

        return NS_ERROR;
    }

    /*
     * Set the default time zone to UTC.
     */

    if (Dbi_ExecDirect(handle, "set session time_zone='+0:00'") != NS_OK) {
        Dbi_LogException(handle, Error);
    }
    

    /*
     * Enable the 'turn off the bugs' options.
     */

    if (Dbi_ExecDirect(handle, "set session sql_mode='ansi,traditional'")
            != NS_OK) {
        Dbi_LogException(handle, Error);
    }

    /*
     * Extra handle info to help with debuging.
     */

    Dbi_SetException(handle, "00000", "version=%s host=%s",
                     mysql_get_server_info(conn),
                     mysql_get_host_info(conn));

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
Bind(Tcl_DString *ds, CONST char *name, int bindIdx)
{
    Tcl_DStringAppend(ds, "?", TCL_INDEX_NONE);
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
    MYSQL_FIELD   *field;
    int            i;

    InitThread();

    if (stmt->driverData == NULL) {

        if ((st = mysql_stmt_init(myHandle->conn)) == NULL) {
            Ns_Fatal("dbimy: Prepare: out of memory allocating statement.");
        }
        if (mysql_stmt_prepare(st, stmt->sql, stmt->length)) {
            MyException(handle, st);
            mysql_stmt_close(st);
            return NS_ERROR;
        }
        *numVarsPtr = mysql_stmt_param_count(st);
        *numColsPtr = mysql_stmt_field_count(st);

        /*
         * Figure out binary/text types for each column.
         */

        meta = NULL;

        if (*numColsPtr > 0) {

            if ((meta = mysql_stmt_result_metadata(st)) == NULL) {
                MyException(handle, st);
                (void) mysql_stmt_close(st);
                return NS_ERROR;
            }

            for (i = 0; i < *numColsPtr; i++) {

                if ((field = mysql_fetch_field_direct(meta, i)) == NULL) {
                    MyException(handle, myStmt->st);
                    (void) mysql_stmt_close(st);
                    mysql_free_result(meta);
                    return NS_ERROR;
                }
                switch (field->type) {
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB:
                    myHandle->bind[i].buffer_type = MYSQL_TYPE_BLOB;
                    break;
                default:
                    myHandle->bind[i].buffer_type = MYSQL_TYPE_STRING;
                }
            }
        }

        myStmt = ns_malloc(sizeof(MyStatement));
        myStmt->st = st;
        myStmt->meta = meta;
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
     * Execute the statment and tell mysql where to bind the result data.
     */

    if (mysql_stmt_execute(myStmt->st)) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }

    if (mysql_stmt_field_count(myStmt->st)) {

        if (!mysql_embedded()) {
            /* Buffer the entire result set to the client. */
            mysql_stmt_store_result(myStmt->st);
        }

        if (mysql_stmt_bind_result(myStmt->st, myHandle->bind)) {
            MyException(handle, myStmt->st);
            return NS_ERROR;
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NextRow --
 *
 *      Fetch the next row.
 *
 * Results:
 *      NS_OK or NS_ERROR, endPtr set to 1 after last row has been fetched.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NextRow(Dbi_Handle *handle, Dbi_Statement *stmt, int *endPtr)
{
    MyStatement  *myStmt = stmt->driverData;
    int           status = NS_OK;

    switch (mysql_stmt_fetch(myStmt->st)) {

    case MYSQL_NO_DATA:
        *endPtr = 1;
        break;

    case 1:
        MyException(handle, myStmt->st);
        status = NS_ERROR;
        break;

    case 0:
    case MYSQL_DATA_TRUNCATED:
        /* fallthrough */
        break;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ColumnLength --
 *
 *      Return the length of the column value and it's text/binary
 *      type after a NextRow(). Null values are 0 length.
 *
 * Results:
 *      NS_OK;
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ColumnLength(Dbi_Handle *handle, Dbi_Statement *stmt, unsigned int index,
             size_t *lengthPtr, int *binaryPtr)
{
    MyHandle *myHandle = handle->driverData;

    if (myHandle->nulls[index]) {
        /* MySQL sometimes reports spurious lengths for NULLs... */
        *lengthPtr = 0;
    } else {
        *lengthPtr = (size_t) myHandle->lengths[index];
    }
    *binaryPtr = myHandle->bind[index].buffer_type == MYSQL_TYPE_BLOB ? 1 : 0;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ColumnValue --
 *
 *      Fetch the indicated value from the current row.
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
ColumnValue(Dbi_Handle *handle, Dbi_Statement *stmt, unsigned int index,
            char *value, size_t length)
{
    MyHandle              *myHandle = handle->driverData;
    MyStatement           *myStmt   = stmt->driverData;
    MYSQL_BIND             bind;
    my_bool                error;

    memset(&bind, 0, sizeof(bind));
    bind.buffer        = value;
    bind.buffer_length = length;
    bind.error         = &error;
    bind.buffer_type   = myHandle->bind[index].buffer_type;

    error = 0;

    if (mysql_stmt_fetch_column(myStmt->st, &bind, index, 0)) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }

    return NS_OK;
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
ColumnName(Dbi_Handle *handle, Dbi_Statement *stmt, unsigned int index,
           CONST char **columnPtr)
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
    Tcl_DString ds;

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
            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "savepoint s%u", depth);

            if (mysql_query(myHandle->conn, ds.string)) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                Tcl_DStringFree(&ds);
                return NS_ERROR;
            }
            Tcl_DStringFree(&ds);
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
            Tcl_DStringInit(&ds);
            Ns_DStringPrintf(&ds, "rollback to savepoint s%u", depth);
            if (mysql_query(myHandle->conn, ds.string)) {
                Dbi_SetException(handle, mysql_sqlstate(myHandle->conn),
                                 mysql_error(myHandle->conn));
                Tcl_DStringFree(&ds);
                return NS_ERROR;
            }
            Tcl_DStringFree(&ds);
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

    if (myStmt->st && mysql_stmt_free_result(myStmt->st)) {
        MyException(handle, myStmt->st);
        return NS_ERROR;
    }

    return NS_OK;
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
        Ns_Fatal("dbimy[%s]: CR_OUT_OF_MEMORY: %s",
                 Dbi_PoolName(handle->pool), mysql_stmt_error(st));
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
 *      InitThread is called from Open, Prepare and Exec, the 3 functions
 *      which a thread must call before calling any other dbi functions.
 *
 *      CleanupThread is a Tls callback which gets called only when a
 *      thread exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      MySQL memory is freed on thread exit.
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


/*
 *----------------------------------------------------------------------
 *
 * AtExit --
 *
 *      Cleanup the mysql library when the server exits. This is
 *      important when running the embedded server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Embedded mysql may flush data to disk and close tables cleanly.
 *
 *----------------------------------------------------------------------
 */

static void
AtExit(void *arg)
{
    Ns_Log(Debug, "dbimy: AtExit");
    mysql_library_end();
}
