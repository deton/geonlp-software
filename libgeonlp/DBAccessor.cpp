///
/// @file
/// @brief DBアクセスクラス DBAccessorの実装。
/// @author 国立情報学研究所
///
/// Copyright (c)2010-2013, NII
///
#include <iostream>
#include <cstdlib>
#include <sqlite3.h>
#include <cassert>
#include <string.h>
#include <boost/regex.hpp>
#include <boost/filesystem.hpp>
#include "config.h"
#include "darts.h"
#include "DBAccessor.h"
#include "Util.h"
#ifdef HAVE_LIBDAMS
#include <dams.h>
#endif /* HAVE_LIBDAMS */

// key-value を保存するための構造体
// DBAccesor::updateWordlist で利用する
// 見出し語と単語ID列を管理するために利用する
// 実質的には geonlp::Wordlist と同じだが id を持たない
struct tmp_wordlist {
public:
  std::string key;
  std::string val;
  std::string surface;
  std::string yomi;
public:
  tmp_wordlist(const std::string& k, const std::string& v, const std::string& s, const std::string& y):key(k), val(v), surface(s), yomi(y) {}
};
bool operator<(const tmp_wordlist& kv0, const tmp_wordlist& kv1) {
  return kv0.key < kv1.key;
}
bool operator>(const tmp_wordlist& kv0, const tmp_wordlist& kv1) {
  return kv0.key > kv1.key;
}

namespace geonlp
{
  /// @brief DBオープン。
  ///
  /// DBは読み込み専用でオープンされる。
  /// @exception std::runtime_error オープンに失敗。例外オブジェクトはSqlite3のエラーメッセージを保持する。
  void DBAccessor::open() throw(std::runtime_error) {

#ifdef DEBUG
    fplog = fopen("/tmp/dbaccess.log", "w");
    fprintf(fplog, "sqlite3_open('%s,%s')\n", sqlite3_fname.c_str(), wordlist_fname.c_str());
#endif /* DEBUG */

    int ret;
    ret = sqlite3_open( sqlite3_fname.c_str(),   /* Database filename (UTF-8) */
			    &sqlitep           /* OUT: SQLite db handle */
			    );

    if ( SQLITE_OK != ret){
      throw std::runtime_error(sqlite3_errmsg(sqlitep));
    }
    ret = sqlite3_open( wordlist_fname.c_str(),   /* Database filename (UTF-8) */
			    &wordlistp           /* OUT: SQLite db handle */
			    );

    if ( SQLITE_OK != ret){
      throw std::runtime_error(sqlite3_errmsg(wordlistp));
    }
  }
	
  /// @brief DBクローズ。
  ///
  /// @return sqlite3_close()の戻り値をそのまま返す。正常終了時は SQLITE_OK (=0)。
  /// @note 戻り値が0以外の場合(SQLITE_BUSYなど)には、厳密にいうと再試行をすべきである。
  int DBAccessor::close() {
    int ret;
    ret = sqlite3_close(sqlitep);
    sqlitep = NULL;
    ret = sqlite3_close(wordlistp);
    wordlistp = NULL;
#ifdef DEBUG
    fprintf(fplog, "sqlite3_close()\n");
    fclose(fplog);
#endif /* DEBUG */
    return ret;
  }
	
  /// @brief 引数として渡されたIDを持つ地名語エントリの全ての情報を地名語辞書システムから取得する。
  ///
  /// 一致するエントリが見つからない場合には、戻り値の地名語エントリクラスのget_geonlp_id()が空文字列となる。
  /// @arg @c id 地名語ID (GeonlpID)
  /// @arg ret   結果の地名語オブジェクト
  /// @return 該当する地名語が見つかった場合 true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  // Geoword DBAccessor::findGeowordById(const std::string& id) const
  bool DBAccessor::findGeowordById(const std::string& id, Geoword& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
    std::ostringstream oss;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();

    // キャッシュチェック
    if (DBAccessor::searchGeowordFromCache(id, ret)) {
      return true;
    }

    // DB から検索
    oss << "select * from geoword where geonlp_id = '" << id << "';";
    std::string sql =  oss.str();
    rc = sqlite3_get_table(sqlitep, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertGeowordColumns( azResult, column);
      resultToGeoword( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.initByJson("{\"geonlp_id\":\"\"}");
    }
    sqlite3_free_table(azResult);

    // キャッシュに登録
    DBAccessor::addGeowordToCache(ret);

    return ret.isValid();
  }
	
  /// @brief 引数として渡された辞書IDとentry_idのペアを持つ地名語エントリの情報を DB から取得する
  ///
  /// 一致するエントリが見つからない場合には、戻り値の地名語エントリクラスのget_geonlp_id()が空文字列となる。
  /// @arg @c dictionary_id 辞書ID (DictionaryID)
  /// @arg @c entry_id      地名語ID (EntryID)
  /// @arg ret              地名語エントリクラス
  /// @return 見つかった場合は true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findGeowordByDictionaryIdAndEntryId(int dictionary_id, const std::string& entry_id, Geoword& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
    std::ostringstream oss;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    oss << "SELECT * FROM geoword WHERE dictionary_id = " << dictionary_id << " AND entry_id = '" << entry_id << "';";
    std::string sql =  oss.str();
    rc = sqlite3_get_table(sqlitep, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertGeowordColumns( azResult, column);
      resultToGeoword( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.initByJson("{\"geonlp_id\":\"\"}");
    }
    sqlite3_free_table(azResult);

    return ret.isValid();
  }

  /// @brief 指定した名称を持つ地名語エントリ（複数）の情報を DB から取得する
  ///
  /// wordlist をインデックスとして利用するため、geoword テーブルに変更があった場合は
  /// updateWordlists を実行しておく必要がある
  /// @arg @c surface   地名語の表記
  /// @arg ret 地名語エントリクラスの配列 
  /// @return 見つかった地名語エントリ数
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  int DBAccessor::findGeowordListBySurface(const std::string& surface, std::vector<Geoword>& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    Wordlist wordlist;
    if (!this->findWordlistBySurface(surface, wordlist)) {
      return 0; // 表記が一致する地名語は登録されていない
    }
    ret.clear();
    return this->getGeowordListFromWordlist(wordlist, ret);
  }
	
  /// @brief 辞書一覧を DB から取得する
  /// @arg ret 辞書IDをキー、辞書を値とするマップ 
  /// @return 件数
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  int DBAccessor::getDictionaryList(std::map<int, Dictionary>& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    Dictionary dictionary;
    sqlite3_stmt* stmt;
    int rc;

    ret.clear();

    rc = sqlite3_prepare_v2(sqlitep, "SELECT json FROM dictionary", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      throw SqliteErrException(rc, "Prepare SELECT failed.");
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      std::string json_str = (const char*)(sqlite3_column_text(stmt, 0));
      dictionary.initByJson(json_str);
      ret.insert(std::make_pair(dictionary.get_internal_id(), dictionary));
    }
    
    sqlite3_finalize(stmt);
    return ret.size();
  }

  /// @brief 引数として渡されたIDを持つ辞書の情報を取得する
  ///
  /// @arg @c unsigned int 辞書ID
  /// @arg ret 辞書データ 
  /// @return 見つかった場合は true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findDictionaryById(const int id, Dictionary& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    oss.str("");
    oss << "select * from dictionary where id = " << id << ";";
    std::string sql = oss.str();
    rc = sqlite3_get_table(sqlitep, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertDictionaryColumns( azResult, column);
      resultToDictionary( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.initByJson("{\"id\":0}");
    }
    sqlite3_free_table(azResult);
    
    return ret.isValid();
  }
	
  /// @brief 引数として渡されたユーザコードと辞書コードを持つ辞書の情報を取得する
  ///
  /// @arg @c std::string ユーザコード
  /// @arg @c std::string 辞書コード
  /// @arg ret 辞書データ 
  /// @return 見つかった場合 true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findDictionaryByUserCodeAndCode(const std::string& user_code, const std::string& code, Dictionary& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    oss.str("");
    oss << "select * from dictionary where user_code = '" << user_code << "' AND code = '" << code << "';";
    std::string sql = oss.str();
    rc = sqlite3_get_table(wordlistp, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertDictionaryColumns( azResult, column);
      resultToDictionary( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.initByJson("{'id':0}");
    }
    sqlite3_free_table(azResult);

    return ret.isValid();
  }
	
  /// @brief 全ての単語IDリストを DB から取得する
  ///
  /// @return 単語IDリストクラスのリスト
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findAllWordlist(std::vector<Wordlist>& wordlists) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    Wordlist wordlist;
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();
    oss.str("");
    oss << "select * from wordlist";
    std::string sql = oss.str();
    rc = sqlite3_get_table(wordlistp, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertWordlistColumns( azResult, column);
      for (int i = 1, index = column; i <= row; i++) {
	resultToWordlist( &azResult[index], wordlist);
	wordlists.push_back(wordlist);
	index += column;
      }
    }
    sqlite3_free_table(azResult);
    if (row == 0) return false;
    else return true;
  }

  /// @brief 引数として渡されたIDを持つ単語IDリストの情報を取得する。
  ///
  /// @arg @c unsigned int テーマID
  /// @arg ret 単語IDリストクラス 
  /// @return 見つかった場合は true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findWordlistById(const unsigned int id, Wordlist& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();
    oss.str("");
    oss << "select * from wordlist where id = " << id << ";";
    std::string sql = oss.str();
    rc = sqlite3_get_table(wordlistp, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertWordlistColumns( azResult, column);
      resultToWordlist( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.set_surface("");
    }
    sqlite3_free_table(azResult);

    return ret.isValid();
  }

  /// @brief 引数として渡された見出し語を持つ単語IDリストの情報を取得する。
  ///
  /// @arg @c std::string 見出し語
  /// @arg ret 単語IDリストクラス
  /// @return 見つかった場合は true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findWordlistBySurface(const std::string& surface, Wordlist& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();

#ifdef HAVE_LIBDAMS
    std::string standardized_surface(damswrapper::get_standardized_string(surface));
    std::string sql = "select * from wordlist where key = '" + standardized_surface + "';";
#else
    std::string sql = "select * from wordlist where key = '" + surface + "';";
#endif /* HAVE_LIBDAMS */

    rc = sqlite3_get_table(wordlistp, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertWordlistColumns( azResult, column);
      resultToWordlist( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.set_surface("");
    }
    sqlite3_free_table(azResult);
    return ret.isValid();
  }

  /// @brief 引数として渡された読みを持つ単語IDリストの情報を取得する。
  ///
  /// @arg @c std::string 読み
  /// @arg ret 単語IDリストクラス 
  /// @return 見つかった場合は true
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  bool DBAccessor::findWordlistByYomi(const std::string& yomi, Wordlist& ret) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    char **azResult;
    int row, column, rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();
    std::string sql = "select * from wordlist where yomi = '" + yomi + "';";
    rc = sqlite3_get_table(wordlistp, sql.c_str(), &azResult, &row, &column, &zErrMsg);
#ifdef DEBUG
    fprintf(fplog, "sqlite3_get_table('%s')\n", sql.c_str());
#endif /* DEBUG */
    if( zErrMsg || rc != SQLITE_OK){
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      sqlite3_free_table(azResult);
      throw SqliteErrException( rc, errmsg.c_str());
    }
    if ( row != 0){
      assertWordlistColumns( azResult, column);
      resultToWordlist( &azResult[column], ret);
    } else {
      // 結果が０件の場合
      ret.set_surface("");
    }
    sqlite3_free_table(azResult);
    return ret.isValid();
  }

  /// @brief 地名語を一括でDBにセットする
  ///
  /// @arg @c std::vector<Geoword>& 地名語のセット
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::setGeowords(const std::vector<Geoword>& geowords) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    int rc;
    char *zErrMsg;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();

    // テーブルの作成
    this->createTables();

    /*
    // 既存テーブル上のデータの削除
    rc = sqlite3_exec(wordlistp, "DELETE FROM wordlist;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
    */

    // トランザクションの開始
    // トランザクションがかかっていないと、とてもとても遅くなる。
    rc = sqlite3_exec(sqlitep, "BEGIN;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    // 行の作成ループ
    sqlite3_stmt *stm = NULL;

    rc = sqlite3_prepare(sqlitep, "INSERT OR REPLACE INTO geoword VALUES (?, ?, ?, ?);", -1, &stm, NULL);
    if (rc != SQLITE_OK || !stm) {
      std::string errmsg = "failed to prepare statement.";
      throw SqliteErrException(rc, errmsg.c_str());
    }

    try {
      for (unsigned int i = 0; i < geowords.size(); i++) {
	const Geoword* wp = &(geowords[i]);
	std::string json = wp->toJson(); 
	// パラメータのバインド
	sqlite3_bind_text(stm, 1, wp->get_geonlp_id().c_str(), wp->get_geonlp_id().length(), SQLITE_STATIC);
	sqlite3_bind_int(stm, 2, wp->get_dictionary_id());
	sqlite3_bind_text(stm, 3, wp->get_entry_id().c_str(), wp->get_entry_id().length(), SQLITE_STATIC);
	sqlite3_bind_text(stm, 4, json.c_str(), json.length(), SQLITE_STATIC);

	// 実行
	rc = sqlite3_step(stm);
	if (rc != SQLITE_DONE) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}

	// 最後に挿入したROWIDの取得
	// const long long rowid = sqlite3_last_insert_rowid(dp);
	// _tprintf(_TEXT("ROWID: %I64d\n"), rowid);

	// バインドのリセット
	rc = sqlite3_reset(stm);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
	// リセットしてもバインド変数はリセットされないため
	// 明示的にリセットする必要あり。
	rc = sqlite3_clear_bindings(stm);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
      }
    } catch (SqliteErrException e) {
      sqlite3_finalize(stm);
      throw e;
    }
    sqlite3_finalize(stm);

    // コミット
    rc = sqlite3_exec(sqlitep, "COMMIT;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    return;
  }

  /// @brief 辞書を一括でDBにセットする
  ///
  /// @arg @c std::vector<Dictionary>& 辞書のセット
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::setDictionaries(const std::vector<Dictionary>& dictionaries) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    Dictionary dictionary;
    int rc;
    char *zErrMsg;
    sqlite3_stmt *stmt;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();

    // テーブルの作成
    this->createTables();

    // トランザクションの開始
    // トランザクションがかかっていないと、とてもとても遅くなる。
    rc = sqlite3_prepare_v2(sqlitep, "BEGIN", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      //      std::string errmsg = zErrMsg;
      //      sqlite3_free(zErrMsg);
      //throw SqliteErrException(rc, errmsg.c_str());
      throw SqliteErrException(rc, "BEGIN failed.");
    }
    sqlite3_finalize(stmt);

    // 行の作成ループ
    rc = sqlite3_prepare_v2(sqlitep, "INSERT OR REPLACE INTO dictionary VALUES (?, ?, ?, ?);", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      std::string errmsg = "failed to prepare statement.";
      throw SqliteErrException(rc, errmsg.c_str());
    }

    try {
      for (unsigned int i = 0; i < dictionaries.size(); i++) {
	const Dictionary* wp = &(dictionaries[i]);
	std::string user_code = wp->get_user_code();
	std::string dict_code = wp->get_code();
	const std::string& json = wp->toJson(); 
	// パラメータのバインド
	sqlite3_bind_int(stmt, 1, wp->get_internal_id());
	sqlite3_bind_text(stmt, 2, user_code.c_str(), user_code.length(), SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, dict_code.c_str(), dict_code.length(), SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, json.c_str(), json.length(), SQLITE_STATIC);

	// 実行
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}

	// 最後に挿入したROWIDの取得
	// const long long rowid = sqlite3_last_insert_rowid(dp);
	// _tprintf(_TEXT("ROWID: %I64d\n"), rowid);

	// バインドのリセット
	rc = sqlite3_reset(stmt);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}

	// リセットしてもバインド変数はリセットされないため
	// 明示的にリセットする必要あり。
	rc = sqlite3_clear_bindings(stmt);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(sqlitep);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
      }
    } catch (SqliteErrException e) {
      sqlite3_finalize(stmt);
      throw e;
    }
    sqlite3_finalize(stmt);

    // コミット
    rc = sqlite3_prepare_v2(sqlitep, "COMMIT;", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, "COMMIT failed.");
    }
    sqlite3_finalize(stmt);

    return;
  }

  /// @brief 単語IDリストを一括でDBにセットする
  ///
  /// @arg @c std::vector<Wordlist>& 単語IDリストのセット
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::setWordlists(const std::vector<Wordlist>& wordlists) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    Wordlist wordlist;
    int rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();

    // テーブルの作成
    this->createTables();

    /*
    // 既存テーブル上のデータの削除
    rc = sqlite3_exec(wordlistp, "DELETE FROM wordlist;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
    */

    // トランザクションの開始
    // トランザクションがかかっていないと、とてもとても遅くなる。
    rc = sqlite3_exec(wordlistp, "BEGIN;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    // 行の作成ループ
    sqlite3_stmt *stm = NULL;

    rc = sqlite3_prepare(wordlistp, "INSERT OR REPLACE INTO wordlist VALUES (?, ?, ?, ?, ?);", -1, &stm, NULL);
    if (rc != SQLITE_OK || !stm) {
      std::string errmsg = "failed to prepare statement.";
      throw SqliteErrException(rc, errmsg.c_str());
    }

    try {
      for (unsigned int i = 0; i < wordlists.size(); i++) {
	const Wordlist* wp = &(wordlists[i]);
	// パラメータのバインド
	sqlite3_bind_int(stm, 1, wp->get_id());
	sqlite3_bind_text(stm, 2, wp->get_surface().c_str(), wp->get_surface().length(), SQLITE_STATIC);
	sqlite3_bind_text(stm, 3, wp->get_idlist().c_str(), wp->get_idlist().length(), SQLITE_STATIC);
	sqlite3_bind_text(stm, 4, wp->get_yomi().c_str(), wp->get_yomi().length(), SQLITE_STATIC);

	// 実行
	rc = sqlite3_step(stm);
	if (rc != SQLITE_DONE) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}

	// 最後に挿入したROWIDの取得
	// const long long rowid = sqlite3_last_insert_rowid(dp);
	// _tprintf(_TEXT("ROWID: %I64d\n"), rowid);

	// バインドのリセット
	rc = sqlite3_reset(stm);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
	// リセットしてもバインド変数はリセットされないため
	// 明示的にリセットする必要あり。
	rc = sqlite3_clear_bindings(stm);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
      }
    } catch (SqliteErrException e) {
      sqlite3_finalize(stm);
      throw e;
    }
    sqlite3_finalize(stm);

    // コミット
    rc = sqlite3_exec(wordlistp, "COMMIT;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    return;
  }

  /// @brief 地名語テーブルをクリアする
  ///
  /// @arg    なし
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::clearGeowords() const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    int rc;
    char *zErrMsg;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    this->createTables(); // テーブルが存在していなければ作成しておく

    // 既存テーブル上のデータの削除
    rc = sqlite3_exec(sqlitep, "DELETE FROM geoword;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @brief 辞書テーブルをクリアする
  ///
  /// @arg    なし
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::clearDictionaries() const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    int rc;
    char *zErrMsg;
		
    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    this->createTables(); // テーブルが存在していなければ作成しておく

    // 既存テーブル上のデータの削除
    rc = sqlite3_exec(sqlitep, "DELETE FROM dictionary;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @brief 単語IDリストテーブルをクリアする
  ///
  /// @arg    なし
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  void DBAccessor::clearWordlists() const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    std::ostringstream oss;
    int rc;
    char *zErrMsg;
		
    if ( NULL == wordlistp) throw SqliteNotInitializedException();
    this->createTables(); // テーブルが存在していなければ作成しておく

    // 既存テーブル上のデータの削除
    rc = sqlite3_exec(wordlistp, "DELETE FROM wordlist;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @brief 単語IDリストテーブルを更新する
  ///        darts ファイルも更新する
  ///
  /// @arg    なし
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  /// @exception DartsException Darts ファイルへの書き込みでエラー。
  void DBAccessor::updateWordlists(void) const
    throw (SqliteNotInitializedException, SqliteErrException, DartsException)
  {
    std::vector<Wordlist> wordlists;
    this->updateWordlists(wordlists);
  }

  /// @brief 単語IDリストテーブルを更新する（更新結果も取得する）
  ///        darts ファイルも更新する
  ///
  /// @arg    wordlists   更新後の Wordlist を格納する配列
  /// @return なし
  /// @exception SqliteNotInitializedException Sqlite3が未初期化。
  /// @exception SqliteErrException Sqlite3でエラー。
  /// @exception DartsException Darts ファイルへの書き込みでエラー。
  void DBAccessor::updateWordlists(std::vector<Wordlist>& wordlists) const
    throw (SqliteNotInitializedException, SqliteErrException, DartsException)
  {
    std::string empty_str("");
    std::map<std::string, std::vector<std::string> > surface_idlist;
    sqlite3_stmt* stmt;
    Geoword geo_in;
    int rc;
    char *zErrMsg;
		
    if (NULL == sqlitep || NULL == wordlistp) throw SqliteNotInitializedException();
    this->createTables(); // テーブルが存在していなければ作成しておく

    // 既存テーブル上のデータの削除
    this->clearWordlists();

    // 地名語をスキャンして単語リストを構築する
    const char* select_sql = "SELECT geonlp_id, json FROM geoword;";
    rc = sqlite3_prepare_v2(sqlitep, select_sql, -1, &stmt, &select_sql);
    if (rc != SQLITE_OK || !stmt) {
      throw SqliteErrException(rc, "Failed to prepare statement.");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      // std::string geonlp_id = sqlite3_column_text(stmt, 0);
      std::string json_str = (const char*)(sqlite3_column_text(stmt, 1));
      geo_in.initByJson(json_str);
      std::string geonlp_id = geo_in.get_geonlp_id();

      // 可能な全ての表記を登録 
      std::vector<std::string> prefixes = geo_in.get_prefix();
      if (prefixes.size() == 0) prefixes.push_back(empty_str);
      std::vector<std::string> suffixes = geo_in.get_suffix();
      if (suffixes.size() == 0) suffixes.push_back(empty_str);
      std::vector<std::string> prefixes_kana = geo_in.get_prefix_kana();
      if (prefixes_kana.size() == 0) prefixes_kana.push_back(empty_str);
      std::vector<std::string> suffixes_kana = geo_in.get_suffix_kana();
      if (suffixes_kana.size() == 0) suffixes_kana.push_back(empty_str);
      
      const std::string body = geo_in.get_body();
      const std::string body_kana = geo_in.get_body_kana();
      const std::string typical_name = geo_in.get_typical_name();
      int i_prefix = 0;
      int i_suffix = 0;
      for (std::vector<std::string>::iterator it_prefix = prefixes.begin(); it_prefix != prefixes.end(); it_prefix++) {
	for (std::vector<std::string>::iterator it_suffix = suffixes.begin(); it_suffix != suffixes.end(); it_suffix++) {
	  std::string surface = (*it_prefix) + body + (*it_suffix);
	  std::string yomi  = "";
	  if (body_kana.length() > 0) {
	    if (i_prefix < prefixes_kana.size()) {
	      yomi += prefixes_kana[i_prefix];
	    } else {
	      yomi += "";
	    }
	    yomi += body_kana;
	    if (i_suffix < suffixes_kana.size()) {
	      yomi += suffixes_kana[i_suffix];
	    } else {
	      yomi += "";
	    }
	  }

#ifdef HAVE_LIBDAMS
	  std::string standardized = std::string(damswrapper::get_standardized_string(surface));
#else
	  std::string standardized = surface;
#endif /* HAVE_LIBDAMS */
	  if (surface_idlist[standardized].size() == 0) {
	    surface_idlist[standardized].push_back("");
	    surface_idlist[standardized].push_back(surface);
	    surface_idlist[standardized].push_back(yomi);
	  } else {
	    surface_idlist[standardized][0] += "/";
	  }
	  surface_idlist[standardized][0] += geonlp_id + ":" + typical_name;

	  if (yomi.length() > 0) {
	    if (surface_idlist[yomi].size() == 0) {
	      surface_idlist[yomi].push_back("");
	      surface_idlist[yomi].push_back(surface);
	      surface_idlist[yomi].push_back(yomi);
	    } else {
	      surface_idlist[yomi][0] += "/";
	    }
	    surface_idlist[yomi][0] += geonlp_id + ":" + typical_name;
	  }
	  
	  i_suffix++;
	}
	i_prefix++;
      }
    }
    // select 終了
    sqlite3_finalize(stmt);

    // デバグ用
#ifdef DEBUG
    for (std::map<std::string, std::vector<std::string> >::iterator it = surface_idlist.begin(); it != surface_idlist.end(); it++) {
      const std::vector<std::string>& elem = (*it).second;
      std::cout << "'" << (*it).first << "' => '" << elem[0] << "', '" << elem[1] << "', '" << elem[2] << "'\n";
    }
#endif /* DEBUG */

    // 文字コード昇順に並べ替え（darts は文字コード順である必要があるので）
    std::vector<tmp_wordlist> tmp_wordlists;
    for (std::map<std::string, std::vector<std::string> >::iterator it = surface_idlist.begin(); it != surface_idlist.end(); it++) {
      const std::vector<std::string>& elem = (*it).second;
      tmp_wordlists.push_back(tmp_wordlist( (*it).first, elem[0], elem[1], elem[2]));  // 標準表記, idlist, 表記, 読み
    }
    std::sort(tmp_wordlists.begin(), tmp_wordlists.end());

    // darts 用テーブル作成
    Wordlist* wp;
    std::vector<const char*> keys;
    for (int seq_id = 0; seq_id < tmp_wordlists.size(); seq_id++) {
      tmp_wordlist& w = tmp_wordlists[seq_id];
      char* tmp = new char[w.key.size() + 1];
      std::strcpy(tmp, w.key.c_str());
      keys.push_back(tmp);
      wordlists.push_back(Wordlist(seq_id, w.key, w.surface, w.val, w.yomi));
    }

    // darts 構築とファイルへの保存
    std::string tmp_darts_fname = this->tmpDartsFilename();
    Darts::DoubleArray da;
    if (da.build(keys.size(), &keys[0], 0, 0, 0) != 0) 
      throw DartsException("Cannot build darts table.");
    if (da.save(tmp_darts_fname.c_str()) != 0)
    //    if (da.save(this->darts_fname.c_str()) != 0)
      throw DartsException("Cannot save darts index to temporary file.");

    // darts 用テーブル（オンメモリ）の削除
    for (int i = 0; i < keys.size(); i++) delete[] keys[i];
    
    // Wordlist を DB に書き込むトランザクションの開始
    rc = sqlite3_prepare_v2(wordlistp, "BEGIN", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    rc = sqlite3_step(stmt);
    if (SQLITE_DONE != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    sqlite3_finalize(stmt);

    // 一時 Wordlist テーブル作成
    this->createTmpWordlistTable();

    // 単語リストを一時テーブルに登録
    const char* insert_sql = "INSERT INTO wordlist_tmp VALUES (?,?,?,?,?)"; // id, surface, idlist, yomi
    rc = sqlite3_prepare_v2(wordlistp, insert_sql, -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    
    try {
      unsigned int id = 0;
      for (std::vector<Wordlist>::iterator it = wordlists.begin(); it != wordlists.end(); it++) {
	sqlite3_bind_int(stmt, 1, (*it).get_id());
	sqlite3_bind_text(stmt, 2, (*it).get_key().c_str(), (*it).get_key().length(), SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, (*it).get_surface().c_str(), (*it).get_surface().length(), SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, (*it).get_idlist().c_str(), (*it).get_idlist().length(), SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, (*it).get_yomi().c_str(), (*it).get_yomi().length(), SQLITE_STATIC);
	
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
	rc = sqlite3_reset(stmt);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
	rc = sqlite3_clear_bindings(stmt);
	if (rc != SQLITE_OK) {
	  std::string errmsg = sqlite3_errmsg(wordlistp);
	  throw SqliteErrException(rc, errmsg.c_str());
	}
      }
    } catch (SqliteErrException e) {
      sqlite3_finalize(stmt);
      throw e;
    }
    sqlite3_finalize(stmt);

    // 一時テーブルを正規テーブルにコピー
    rc = sqlite3_prepare_v2(wordlistp, "DROP TABLE wordlist", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    rc = sqlite3_step(stmt);
    if (SQLITE_DONE != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(wordlistp, "ALTER TABLE wordlist_tmp RENAME TO wordlist", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    rc = sqlite3_step(stmt);
    if (SQLITE_DONE != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    sqlite3_finalize(stmt);

    // コミット
    rc = sqlite3_prepare_v2(wordlistp, "COMMIT", -1, &stmt, NULL);
    if (SQLITE_OK != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    rc = sqlite3_step(stmt);
    if (SQLITE_DONE != rc) {
      throw SqliteErrException(rc, sqlite3_errmsg(wordlistp));
    }
    sqlite3_finalize(stmt);

    // 一時ファイルを正規ファイルに移動
    boost::filesystem::path tmppath(this->tmpDartsFilename());
    boost::filesystem::path regpath(this->darts_fname);
    boost::filesystem::remove(regpath);
    boost::filesystem::rename(tmppath, regpath);
  }

  /// @brief geowordテーブルから得られた情報が、期待する順序でカラムが並んでいることを確認する
  ///
  /// @note DBのカラム並びが変わらない限り得られる情報におけるカラム並びは不変なので、期待する順序でカラムが並んでいることを
  /// 前提としてよいが、DBのカラム並びが変わってしまった場合に備えて用意された関数。
  int DBAccessor::assertGeowordColumns( char**azResult, int column) const
    throw (SqliteNotInitializedException, SqliteErrException)
  {
    /*
      static const char* colName[] = {
      "id", "geoword", "yomi", "pron", "desinence", "geoword_full",
      "yomi_full", "pron_full", "upper_geowords", "latitude", "longitude",
      "theme_id", "code", "state_code", "city_code", "address", "note"
    };
    */
    static const char* colName[] = {
      "geonlp_id", "dictionary_id", "entry_id", "json"
    };
    assert( column == 4);
    for (int i= 0; i < 4; i++) {
      assert( strcmp( azResult[i], colName[i])== 0);
    }
    return 0;
  }

  /// @brief dictionaryテーブルから得られた情報が、期待する順序でカラムが並んでいることを確認する
  int DBAccessor::assertDictionaryColumns( char**azResult, int column) const
      throw (SqliteNotInitializedException, SqliteErrException)
  {
    static const char* colName[] = {
      "id", "user_code", "code", "json"
    };
    assert( column == 4);
    for (int i= 0; i < 4; i++) {
      assert( strcmp( azResult[i], colName[i])== 0);
    }
    return 0;
  }

  /// @brief wordlistテーブルから得られた情報が、期待する順序でカラムが並んでいることを確認する
  int DBAccessor::assertWordlistColumns(char** azResult, int column) const
      throw (SqliteNotInitializedException, SqliteErrException)
  {
    static const char* colName[] = {
      "id", "key", "surface", "idlist", "yomi"
    };
    assert( column == 5);
    for (int i = 0; i < 5; i++) {
      assert( strcmp( azResult[i], colName[i])== 0);
    }
    return 0;
  }
	
  /// @brief geowordテーブルから得られた情報を地名語エントリクラスに変換する
  ///
  /// @arg @c azResult [in] Sqlite3から得られた情報
  /// @arg @c out [out] 地名語エントリクラス
  void DBAccessor::resultToGeoword( char **azResult, Geoword& out) const
    throw (picojson::PicojsonException)
  {
    out.initByJson(azResult[3]); // "json" フィールドを parse する
  }

  /// @brief dictionaryテーブルから得られた情報を地名語エントリクラスに変換する
  ///
  /// @arg @c azResult [in] Sqlite3から得られた情報
  /// @arg @c out [out] 地名語エントリクラス
  void DBAccessor::resultToDictionary( char **azResult, Dictionary& out) const
    throw (picojson::PicojsonException)
  {
    out.initByJson(azResult[3]); // "json" フィールドを parse する
  }

  /// @brief wordlistテーブルから得られた情報を単語IDリストクラスに変換する。
  ///
  /// @arg @c azResult [in] Sqlite3から得られた情報
  /// @arg @c out [out] 単語IDリストクラス
  void DBAccessor::resultToWordlist( char **azResult, Wordlist& out) const
  {
    out.set_id( *azResult ? atoi(*azResult) : 0); azResult++;
    out.set_key( *azResult ? *azResult : ""); azResult++;
    out.set_surface( *azResult ? *azResult : ""); azResult++;
    out.set_idlist( *azResult ? *azResult : ""); azResult++;
    out.set_yomi( *azResult ? *azResult : ""); azResult++;
  }

  /// @breaf geoword, dictionary, wordlist テーブルを作成する
  /// 既にテーブルが存在すれば何もしない
  void DBAccessor::createTables() const
    throw (SqliteNotInitializedException, SqliteErrException) {
    int rc;
    char *zErrMsg;

    if ( NULL == sqlitep) throw SqliteNotInitializedException();
    if ( NULL == wordlistp) throw SqliteNotInitializedException();

    rc = sqlite3_exec(sqlitep, "CREATE TABLE IF NOT EXISTS geoword(geonlp_id VARCHAR PRIMARY KEY, dictionary_id INTEGER, entry_id INTEGER, json VARCHAR);", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    rc = sqlite3_exec(sqlitep, "CREATE TABLE IF NOT EXISTS dictionary(id INTEGER PRIMARY KEY, user_code VARCHAR, code VARCHAR, json VARCHAR);", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }

    rc = sqlite3_exec(wordlistp, "CREATE TABLE IF NOT EXISTS wordlist(id INTEGER PRIMARY KEY, key VARCHAR, surface VARCHAR, idlist VARCHAR, yomi VARCHAR);", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @brief wordlist 更新時の一時テーブルを削除する
  void DBAccessor::dropTmpWordlistTable(void) const
    throw (SqliteNotInitializedException, SqliteErrException) {
    int rc;
    char *zErrMsg;

    if ( NULL == wordlistp) throw SqliteNotInitializedException();

    // 一時テーブルが存在すれば削除
    rc = sqlite3_exec(wordlistp, "DROP TABLE IF EXISTS wordlist_tmp;", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @breaf wordlist 更新時の一時テーブルを生成する
  /// 既にテーブルが存在していても生成しなおす
  void DBAccessor::createTmpWordlistTable(void) const
    throw (SqliteNotInitializedException, SqliteErrException) {
    int rc;
    char *zErrMsg;

    if ( NULL == wordlistp) throw SqliteNotInitializedException();

    // 一時テーブルが残っていれば削除
    this->dropTmpWordlistTable();

    // wordlist と同じスキーマを持つテーブルを作成する
    // create table .. as select は PRIMARY KEY がコピーされないので不可
    rc = sqlite3_exec(wordlistp, "CREATE TABLE wordlist_tmp(id INTEGER PRIMARY KEY, key VARCHAR, surface VARCHAR, idlist VARCHAR, yomi VARCHAR);", NULL, NULL, &zErrMsg);
    if (zErrMsg || rc != SQLITE_OK) {
      std::string errmsg = zErrMsg;
      sqlite3_free(zErrMsg);
      throw SqliteErrException(rc, errmsg.c_str());
    }
  }

  /// @brief wordlist に含まれる ID を持つ Geoword をデータベースから取得する
  /// @arg @c wordlist  ID リストを含む wordlist
  /// @arg ret          地名語エントリのリスト
  /// @arg limit        取得する Geoword 件数の上限、0 の場合全件
  /// @return           取得した件数
  int DBAccessor::getGeowordListFromWordlist(const Wordlist& wordlist, std::vector<Geoword>& ret, int limit) const {
    const std::string& idlist = wordlist.get_idlist();
    Geoword geoword;

    ret.clear();
    boost::regex pattern = boost::regex("([^\\/:]+):([^\\/:]*)", boost::regex_constants::egrep);

    for (boost::sregex_iterator it = boost::make_regex_iterator(idlist, pattern); it != boost::sregex_iterator(); it++) {
      std::string geonlp_id = (*it)[1]; // geonlp_id
      // (*it)[2]; // typical name
      if (this->findGeowordById(geonlp_id, geoword)) ret.push_back(geoword);
      if (limit > 0 && ret.size() >= limit) break;
    }
    return ret.size();
  }

  /// キャッシュ関連メソッド
  std::map<std::string, Geoword> DBAccessor::geoword_cache;

  bool DBAccessor::searchGeowordFromCache(const std::string& geonlp_id, Geoword& geoword) {
    std::map<std::string, Geoword>::iterator it = DBAccessor::geoword_cache.find(geonlp_id);
    if (it == DBAccessor::geoword_cache.end()) return false;
    geoword = (*it).second;
    return true;
  }

  void DBAccessor::addGeowordToCache(const Geoword& geoword) {
    if (!geoword.isValid()) return;
    if (DBAccessor::geoword_cache.size() > GEOWORD_CACHE_SIZE) DBAccessor::clearGeowordCache();
    const std::string& geonlp_id = geoword.get_geonlp_id();
    DBAccessor::geoword_cache[geonlp_id] = geoword;
  }

  void DBAccessor::clearGeowordCache(void) {
    DBAccessor::geoword_cache.clear();
  }

}
