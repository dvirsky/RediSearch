#include "doc_table.h"
#include "redis_index.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include "util/logging.h"
#include <stdio.h>

/**
* Format redis key for a term.
* TODO: Add index name to it
*/
RedisModuleString *fmtRedisTermKey(RedisSearchCtx *ctx, const char *term, size_t len) {
  return RedisModule_CreateStringPrintf(ctx->redisCtx, TERM_KEY_FORMAT, ctx->spec->name, len, term);
}

RedisModuleString *fmtRedisSkipIndexKey(RedisSearchCtx *ctx, const char *term, size_t len) {
  return RedisModule_CreateStringPrintf(ctx->redisCtx, SKIPINDEX_KEY_FORMAT, ctx->spec->name, len,
                                        term);
}

RedisModuleString *fmtRedisScoreIndexKey(RedisSearchCtx *ctx, const char *term, size_t len) {
  return RedisModule_CreateStringPrintf(ctx->redisCtx, SCOREINDEX_KEY_FORMAT, ctx->spec->name, len,
                                        term);
}
/**
* Open a redis index writer on a redis key
*/
IndexWriter *Redis_OpenWriter(RedisSearchCtx *ctx, const char *term, size_t len) {
  // Open the index writer
  RedisModuleString *termKey = fmtRedisTermKey(ctx, term, len);
  BufferWriter bw = NewRedisWriter(ctx->redisCtx, termKey, 0);
  RedisModule_FreeString(ctx->redisCtx, termKey);
  // Open the skip index writer
  termKey = fmtRedisSkipIndexKey(ctx, term, len);
  Buffer *sb = NewRedisBuffer(ctx->redisCtx, termKey, BUFFER_WRITE | BUFFER_LAZY_ALLOC);
  BufferWriter skw = {
      sb, redisWriterWrite, redisWriterTruncate, RedisBufferFree,
  };
  RedisModule_FreeString(ctx->redisCtx, termKey);

  if (sb->cap > sizeof(u_int32_t)) {
    u_int32_t len;

    BufferRead(sb, &len, sizeof(len));
    BufferSeek(sb, sizeof(len) + len * sizeof(SkipEntry));
  }

  termKey = fmtRedisScoreIndexKey(ctx, term, len);

  // Open the score index writer
  ScoreIndexWriter scw =
      NewScoreIndexWriter(NewRedisWriter(ctx->redisCtx, fmtRedisScoreIndexKey(ctx, term, len), 1));
  RedisModule_FreeString(ctx->redisCtx, termKey);
  IndexWriter *w = NewIndexWriterBuf(bw, skw, scw, ctx->spec->flags);
  return w;
}

void Redis_CloseWriter(IndexWriter *w) {
  IW_Close(w);
  RedisBufferFree(w->bw.buf);
  RedisBufferFree(w->skipIndexWriter.buf);
  RedisBufferFree(w->scoreWriter.bw.buf);
  free(w);
}

SkipIndex *LoadRedisSkipIndex(RedisSearchCtx *ctx, const char *term, size_t len) {
  Buffer *b = NewRedisBuffer(ctx->redisCtx, fmtRedisSkipIndexKey(ctx, term, len), BUFFER_READ);
  if (b && b->cap > sizeof(SkipEntry)) {
    SkipIndex *si = malloc(sizeof(SkipIndex));
    BufferRead(b, &si->len, sizeof(si->len));
    si->entries = (SkipEntry *)b->pos;

    RedisBufferFree(b);
    return si;
  }
  RedisBufferFree(b);
  return NULL;
}

ScoreIndex *LoadRedisScoreIndex(RedisSearchCtx *ctx, const char *term, size_t len) {
  Buffer *b = NewRedisBuffer(ctx->redisCtx, fmtRedisScoreIndexKey(ctx, term, len), BUFFER_READ);
  if (b == NULL || b->cap <= sizeof(ScoreIndexEntry)) {
    return NULL;
  }
  return NewScoreIndex(b);
}

IndexReader *Redis_OpenReader(RedisSearchCtx *ctx, const char *term, size_t len, DocTable *dt,
                              int singleWordMode, u_char fieldMask) {
  Buffer *b = NewRedisBuffer(ctx->redisCtx, fmtRedisTermKey(ctx, term, len), BUFFER_READ);
  if (b == NULL) {  // not found
    return NULL;
  }
  SkipIndex *si = NULL;
  ScoreIndex *sci = NULL;
  if (singleWordMode) {
    sci = LoadRedisScoreIndex(ctx, term, len);
  } else {
    si = LoadRedisSkipIndex(ctx, term, len);
  }

  return NewIndexReaderBuf(b, si, dt, singleWordMode, sci, fieldMask, ctx->spec->flags,
                           NewTerm((char *)term));
}

void Redis_CloseReader(IndexReader *r) {
  // we don't call IR_Free because it frees the underlying memory right now

  RedisBufferFree(r->buf);

  if (r->skipIdx != NULL) {
    free(r->skipIdx);
  }
  if (r->scoreIndex != NULL) {
    ScoreIndex_Free(r->scoreIndex);
  }
  free(r);
}

void Document_Free(Document doc) {
  free(doc.fields);
}

int Redis_LoadDocument(RedisSearchCtx *ctx, RedisModuleString *key, Document *doc) {
  RedisModuleCallReply *rep = RedisModule_Call(ctx->redisCtx, "HGETALL", "s", key);
  RMUTIL_ASSERT_NOERROR(ctx->redisCtx, rep);
  if (RedisModule_CallReplyType(rep) == REDISMODULE_REPLY_NULL) {
    return REDISMODULE_ERR;
  }

  size_t len = RedisModule_CallReplyLength(rep);
  // Zero means the document does not exist in redis
  if (len == 0) {
    return REDISMODULE_ERR;
  }
  doc->fields = calloc(len / 2, sizeof(DocumentField));
  doc->numFields = len / 2;
  int n = 0;
  RedisModuleCallReply *k, *v;
  for (int i = 0; i < len; i += 2, ++n) {
    k = RedisModule_CallReplyArrayElement(rep, i);
    v = RedisModule_CallReplyArrayElement(rep, i + 1);
    doc->fields[n].name = RedisModule_CreateStringFromCallReply(k);
    doc->fields[n].text = RedisModule_CreateStringFromCallReply(v);
  }

  return REDISMODULE_OK;
}

Document NewDocument(RedisModuleString *docKey, double score, int numFields, const char *lang,
                     const char *payload, size_t payloadSize) {
  Document doc;
  doc.docKey = docKey;
  doc.score = (float)score;
  doc.numFields = numFields;
  doc.fields = calloc(doc.numFields, sizeof(DocumentField));
  doc.language = lang;
  doc.payload = payload;
  doc.payloadSize = payloadSize;

  return doc;
}

Document *Redis_LoadDocuments(RedisSearchCtx *ctx, RedisModuleString **keys, int numKeys,
                              int *nump) {
  Document *docs = calloc(numKeys, sizeof(Document));
  int n = 0;

  for (int i = 0; i < numKeys; i++) {
    Redis_LoadDocument(ctx, keys[i], &docs[n]);
    docs[n++].docKey = keys[i];
  }

  *nump = n;
  return docs;
}

int Redis_SaveDocument(RedisSearchCtx *ctx, Document *doc) {

  RedisModuleKey *k =
      RedisModule_OpenKey(ctx->redisCtx, doc->docKey, REDISMODULE_WRITE | REDISMODULE_READ);
  if (k == NULL || (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_EMPTY &&
                    RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_HASH)) {
    return REDISMODULE_ERR;
  }

  for (int i = 0; i < doc->numFields; i++) {
    RedisModule_HashSet(k, REDISMODULE_HASH_NONE, doc->fields[i].name, doc->fields[i].text, NULL);
  }
  return REDISMODULE_OK;
}

int Redis_ScanKeys(RedisModuleCtx *ctx, const char *prefix, ScanFunc f, void *opaque) {
  long long ptr = 0;

  int num = 0;
  do {
    RedisModuleString *sptr = RedisModule_CreateStringFromLongLong(ctx, ptr);
    RedisModuleCallReply *r =
        RedisModule_Call(ctx, "SCAN", "scccc", sptr, "MATCH", prefix, "COUNT", "100");
    RedisModule_FreeString(ctx, sptr);
    if (r == NULL || RedisModule_CallReplyType(r) == REDISMODULE_REPLY_ERROR) {
      return num;
    }

    if (RedisModule_CallReplyLength(r) < 1) {
      break;
    }

    sptr = RedisModule_CreateStringFromCallReply(RedisModule_CallReplyArrayElement(r, 0));
    RedisModule_StringToLongLong(sptr, &ptr);
    RedisModule_FreeString(ctx, sptr);
    // printf("ptr: %s %lld\n",
    // RedisModule_CallReplyStringPtr(RedisModule_CallReplyArrayElement(r, 0),
    // NULL), ptr);
    if (RedisModule_CallReplyLength(r) == 2) {
      RedisModuleCallReply *keys = RedisModule_CallReplyArrayElement(r, 1);
      size_t nks = RedisModule_CallReplyLength(keys);

      for (size_t i = 0; i < nks; i++) {
        RedisModuleString *kn =
            RedisModule_CreateStringFromCallReply(RedisModule_CallReplyArrayElement(keys, i));
        if (f(ctx, kn, opaque) != REDISMODULE_OK) goto end;

        // RedisModule_FreeString(ctx, kn);
        if (++num % 10000 == 0) {
          LG_DEBUG("Scanned %d keys", num);
        }
      }

      // RedisModule_FreeCallReply(keys);
    }

    RedisModule_FreeCallReply(r);

  } while (ptr);
end:
  return num;
}

int Redis_OptimizeScanHandler(RedisModuleCtx *ctx, RedisModuleString *kn, void *opaque) {
  // extract the term from the key
  RedisSearchCtx *sctx = opaque;
  RedisModuleString *pf = fmtRedisTermKey(sctx, "", 0);
  size_t pflen, len;
  const char *prefix = RedisModule_StringPtrLen(pf, &pflen);

  char *k = (char *)RedisModule_StringPtrLen(kn, &len);
  k += pflen;

  // Open the index writer for the term
  IndexWriter *w = Redis_OpenWriter(sctx, k, len - pflen);
  if (w) {
    // Truncate the main index buffer to its final size
    w->bw.Truncate(w->bw.buf, 0);
    sctx->spec->stats.invertedCap += w->bw.buf->cap;
    sctx->spec->stats.invertedSize += w->bw.buf->offset;

    // for small entries, delete the score index
    if (w->ndocs < SCOREINDEX_DELETE_THRESHOLD) {
      RedisBufferCtx *bctx = w->scoreWriter.bw.buf->ctx;
      RedisModule_DeleteKey(bctx->key);
      RedisModule_CloseKey(bctx->key);
      bctx->key = NULL;
    } else {
      // truncate the score index to its final size
      w->scoreWriter.bw.Truncate(w->scoreWriter.bw.buf, 0);
      sctx->spec->stats.scoreIndexesSize += w->scoreWriter.bw.buf->cap;
    }

    // truncate the skip index
    w->skipIndexWriter.Truncate(w->skipIndexWriter.buf, 0);
    sctx->spec->stats.skipIndexesSize += w->skipIndexWriter.buf->cap;

    Redis_CloseWriter(w);
  }

  RedisModule_FreeString(ctx, pf);

  return REDISMODULE_OK;
}

int Redis_DropScanHandler(RedisModuleCtx *ctx, RedisModuleString *kn, void *opaque) {
  // extract the term from the key
  RedisSearchCtx *sctx = opaque;
  RedisModuleString *pf = fmtRedisTermKey(sctx, "", 0);
  size_t pflen, len;
  const char *prefix = RedisModule_StringPtrLen(pf, &pflen);

  char *k = (char *)RedisModule_StringPtrLen(kn, &len);
  k += pflen;
  // char *term = strndup(k, len - pflen);

  RedisModuleString *sck = fmtRedisScoreIndexKey(sctx, k, len - pflen);
  RedisModuleString *sik = fmtRedisSkipIndexKey(sctx, k, len - pflen);

  RedisModule_Call(ctx, "DEL", "sss", kn, sck, sik);

  RedisModule_FreeString(ctx, sck);
  RedisModule_FreeString(ctx, sik);
  // free(term);

  return REDISMODULE_OK;
}

int Redis_DropIndex(RedisSearchCtx *ctx, int deleteDocuments) {

  if (deleteDocuments) {

    DocTable *dt = &ctx->spec->docs;

    for (size_t i = 1; i < dt->size; i++) {
      RedisModuleKey *k = RedisModule_OpenKey(
          ctx->redisCtx,
          RedisModule_CreateString(ctx->redisCtx, dt->docs[i].key, strlen(dt->docs[i].key)),
          REDISMODULE_WRITE);

      if (k != NULL) {
        RedisModule_DeleteKey(k);
        RedisModule_CloseKey(k);
      }
    }
  }

  RedisModuleString *pf = fmtRedisTermKey(ctx, "*", 1);
  const char *prefix = RedisModule_StringPtrLen(pf, NULL);

  // // Delete the actual index sub keys
  Redis_ScanKeys(ctx->redisCtx, prefix, Redis_DropScanHandler, ctx);

  // Delete the index spec
  RedisModuleKey *k = RedisModule_OpenKey(
      ctx->redisCtx,
      RedisModule_CreateStringPrintf(ctx->redisCtx, INDEX_SPEC_KEY_FMT, ctx->spec->name),
      REDISMODULE_WRITE);
  if (k != NULL) {
    RedisModule_DeleteKey(k);
    RedisModule_CloseKey(k);
    return REDISMODULE_OK;
  }

  return REDISMODULE_ERR;
}