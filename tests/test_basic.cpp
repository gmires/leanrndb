#include <iostream>
#include <string>
#include <cassert>
#include <cstdlib>
#include "database.h"
#include "tokenizer.h"
#include "parser.h"
#include "executor.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    std::cout << "  " << name << "... " << std::flush; \
} while(0)

#define PASS() do { \
    tests_passed++; \
    std::cout << "PASS\n"; \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cout << "FAIL at " << __FILE__ << ":" << __LINE__ \
                  << ": '" << #cond << "'\n"; \
        return; \
    } \
} while(0)

static void test_create_table() {
    TEST("create table");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        std::vector<ColumnSchema> cols = {
            {"id", "INT"},
            {"name", "VARCHAR"},
            {"age", "INT"}
        };
        CHECK(db.create_table("users", cols));
        CHECK(!db.create_table("users", cols)); // duplicate

        auto* t = db.find_table("users");
        CHECK(t != nullptr);
        CHECK(t->name == "users");
        CHECK(t->columns.size() == 3);
        CHECK(t->columns[0].name == "id");
        CHECK(t->columns[0].type == "INT");
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_insert_and_select() {
    TEST("insert and select");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        std::vector<ColumnSchema> cols = {
            {"name", "VARCHAR"},
            {"age", "INT"}
        };
        db.create_table("people", cols);

        int64_t r1 = db.insert("people", {Value("Alice"), Value(30)});
        CHECK(r1 == 1);
        int64_t r2 = db.insert("people", {Value("Bob"), Value(25)});
        CHECK(r2 == 2);

        std::vector<Row> rows;
        db.select("people", rows);
        CHECK(rows.size() == 2);
        CHECK(rows[0].values[0].str_val == "Alice");
        CHECK(rows[0].values[1].int_val == 30);
        CHECK(rows[1].values[0].str_val == "Bob");
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_where_clause() {
    TEST("where clause");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        db.create_table("stuff", {{"val", "INT"}});
        db.insert("stuff", {Value(10)});
        db.insert("stuff", {Value(20)});
        db.insert("stuff", {Value(30)});

        std::vector<Row> rows;
        db.select("stuff", rows, "val", ">", Value(15));
        CHECK(rows.size() == 2);
        CHECK(rows[0].values[0].int_val == 10 || rows[0].values[0].int_val == 20);
        // After scan returns all and filters, we should get 20 and 30
        // Count rows with val > 15
        int count = 0;
        for (auto& r : rows) {
            if (r.values[0].int_val > 15) count++;
        }
        CHECK(count == 2);
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_delete() {
    TEST("delete");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        db.create_table("t", {{"x", "INT"}});
        db.insert("t", {Value(1)});
        db.insert("t", {Value(2)});
        db.insert("t", {Value(3)});

        int64_t affected = 0;
        db.delete_rows("t", "x", "=", Value(2), affected);
        CHECK(affected == 1);

        std::vector<Row> rows;
        db.select("t", rows);
        CHECK(rows.size() == 2);
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_index() {
    TEST("index");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        db.create_table("t", {{"name", "VARCHAR"}, {"val", "INT"}});
        db.insert("t", {Value("aaa"), Value(1)});
        db.insert("t", {Value("bbb"), Value(2)});

        db.create_index("idx_name", "t", "name");

        std::vector<Row> rows;
        db.select("t", rows, "name", "=", Value("aaa"));
        CHECK(rows.size() == 1);
        CHECK(rows[0].values[1].int_val == 1);
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_sql_parser() {
    TEST("SQL parser: CREATE TABLE");
    {
        Tokenizer tok("CREATE TABLE users (id INT, name VARCHAR);");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<CreateTableStmt>(stmt));
        auto& ct = std::get<CreateTableStmt>(stmt);
        CHECK(ct.table_name == "users");
        CHECK(ct.columns.size() == 2);
        CHECK(ct.columns[0].name == "id");
        CHECK(ct.columns[1].name == "name");
    }
    PASS();

    TEST("SQL parser: INSERT");
    {
        Tokenizer tok("INSERT INTO t VALUES (42, 'hello');");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<InsertStmt>(stmt));
        auto& ins = std::get<InsertStmt>(stmt);
        CHECK(ins.table_name == "t");
        CHECK(ins.values.size() == 2);
        CHECK(ins.values[0].type == Value::INT_VAL);
        CHECK(ins.values[0].int_val == 42);
        CHECK(ins.values[1].type == Value::STR_VAL);
        CHECK(ins.values[1].str_val == "hello");
    }
    PASS();

    TEST("SQL parser: SELECT with WHERE");
    {
        Tokenizer tok("SELECT name, age FROM users WHERE age > 30;");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<SelectStmt>(stmt));
        auto& sel = std::get<SelectStmt>(stmt);
        CHECK(sel.columns.size() == 2);
        CHECK(sel.columns[0] == "name");
        CHECK(sel.table_name == "users");
        CHECK(sel.where.present);
        CHECK(sel.where.column == "age");
        CHECK(sel.where.op == ">");
        CHECK(sel.where.value.int_val == 30);
    }
    PASS();

    TEST("SQL parser: SELECT *");
    {
        Tokenizer tok("SELECT * FROM t;");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<SelectStmt>(stmt));
        auto& sel = std::get<SelectStmt>(stmt);
        CHECK(sel.columns.empty()); // * = empty
        CHECK(sel.table_name == "t");
    }
    PASS();

    TEST("SQL parser: DELETE");
    {
        Tokenizer tok("DELETE FROM t WHERE id = 5;");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<DeleteStmt>(stmt));
    }
    PASS();

    TEST("SQL parser: UPDATE");
    {
        Tokenizer tok("UPDATE t SET name = 'Bob' WHERE id = 1;");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<UpdateStmt>(stmt));
    }
    PASS();

    TEST("SQL parser: CREATE INDEX");
    {
        Tokenizer tok("CREATE INDEX idx_name ON users (name);");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<CreateIndexStmt>(stmt));
        auto& ci = std::get<CreateIndexStmt>(stmt);
        CHECK(ci.index_name == "idx_name");
        CHECK(ci.table_name == "users");
        CHECK(ci.column == "name");
    }
    PASS();

    TEST("SQL parser: DROP TABLE");
    {
        Tokenizer tok("DROP TABLE users;");
        tok.tokenize_all();
        auto toks = tok.tokens();
        while (!toks.empty() &&
               (toks.back().type == TokenType::END ||
                toks.back().type == TokenType::ERROR))
            toks.pop_back();

        Parser parser(toks);
        auto stmt = parser.parse();
        CHECK(std::holds_alternative<DropTableStmt>(stmt));
        CHECK(std::get<DropTableStmt>(stmt).table_name == "users");
    }
    PASS();
}

static void test_full_sql_flow() {
    TEST("full SQL flow via parser+executor");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);

        auto exec_sql = [&](const std::string& sql) -> QueryResult {
            Tokenizer tok(sql);
            tok.tokenize_all();
            auto toks = tok.tokens();
            while (!toks.empty() &&
                   (toks.back().type == TokenType::END ||
                    toks.back().type == TokenType::ERROR))
                toks.pop_back();
            Parser parser(toks);
            Statement stmt = parser.parse();
            Executor exec(&db);
            return exec.execute(stmt);
        };

        QueryResult r;

        r = exec_sql("CREATE TABLE test (id INT, val VARCHAR);");
        CHECK(r.ok);

        r = exec_sql("INSERT INTO test VALUES (1, 'hello');");
        CHECK(r.ok);
        CHECK(r.affected == 1);

        r = exec_sql("INSERT INTO test VALUES (2, 'world');");
        CHECK(r.ok);

        r = exec_sql("SELECT * FROM test;");
        CHECK(r.ok);
        CHECK(r.rows.size() == 2);
        CHECK(r.rows[0].size() == 2);

        r = exec_sql("SELECT * FROM test WHERE id = 1;");
        CHECK(r.ok);
        CHECK(r.rows.size() == 1);

        r = exec_sql("DELETE FROM test WHERE id = 2;");
        CHECK(r.ok);
        CHECK(r.affected == 1);

        r = exec_sql("SELECT * FROM test;");
        CHECK(r.ok);
        CHECK(r.rows.size() == 1);

        r = exec_sql("DROP TABLE test;");
        CHECK(r.ok);
    }
    std::remove(db_path.c_str());
    PASS();
}

static void test_persistence() {
    TEST("database persistence across open/close");
    std::string db_path = std::tmpnam(nullptr);
    {
        Database db(db_path);
        db.create_table("t", {{"x", "INT"}});
        db.insert("t", {Value(1)});
        db.insert("t", {Value(2)});
    }
    {
        Database db(db_path);
        auto* t = db.find_table("t");
        CHECK(t != nullptr);
        CHECK(t->columns.size() == 1);

        std::vector<Row> rows;
        db.select("t", rows);
        CHECK(rows.size() == 2);
        CHECK(rows[0].values[0].int_val == 1);
        CHECK(rows[1].values[0].int_val == 2);
    }
    std::remove(db_path.c_str());
    PASS();
}

int main() {
    std::cout << "leanrndb tests\n";
    std::cout << "==============\n";

    test_create_table();
    test_insert_and_select();
    test_where_clause();
    test_delete();
    test_index();
    test_sql_parser();
    test_full_sql_flow();
    test_persistence();

    std::cout << "==============\n";
    std::cout << tests_passed << "/" << tests_run << " tests passed\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
