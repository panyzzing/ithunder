#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <locale.h>
#include <sbase.h>
#include <ibase.h>
#include <iconv.h>
#include <chardet.h>
#include <mtask.h>
#include "mtrie.h"
#include "http.h"
#include "mime.h"
#include "iniparser.h"
#include "mutex.h"
#include "iqueue.h"
#include "stime.h"
#include "base64.h"
//#include "base64spiderhtml.h"
#include "timer.h"
#include "logger.h"
#include "xmm.h"
#include "xtask.h"
#include "zstream.h"
#include "base64.h"
#ifndef HTTP_BUF_SIZE
#define HTTP_BUF_SIZE           262144
#endif
#ifndef HTTP_QUERY_MAX
#define HTTP_QUERY_MAX          1024
#endif
#define HTTP_LINE_MAX           8192
#define HTTPD_NUM_PAGE          20
#define HTTPD_NPAGE             20
#define HTTPD_PAGE_MAX          500
#define HTTP_REQ_MAX            8192
#define HTTP_REDIRECT           0x01
#define TASK_WAIT_TIME          1000000
#define LI(xxxx) ((unsigned int)xxxx)
typedef struct _HTTPTASK
{
    int status; 
    int flag;
    int taskid;
    int id;
    CONN *conn;
    XTMETA meta;
    char url[HTTP_URL_MAX];
    char headers[HTTP_LINE_MAX];
}HTTPTASK;
typedef struct _WTASK
{
    int status;
    int id;
    int total;
    int over;
    int nchunk;
    char *p;
    void *mutex;
    CB_DATA *chunk;
    CONN *conn;
    HTTPTASK conns[HTTP_TASK_MAX];
}WTASK;
static SBASE *sbase = NULL;
static SERVICE *spider = NULL;
static SERVICE *httpd = NULL;
static dictionary *dict = NULL;
static void *logger = NULL;
static WTASK *tasks = NULL;
static void *taskqueue = NULL;
static int ntasks = 0;
static int running_status = 1;
static MIME_MAP http_mime_map = {0};
static void *http_headers_map = NULL;
static char *http_default_charset = "UTF-8";
static char *httpd_home = NULL;
static int is_inside_html = 1;
static unsigned char *httpd_index_html_code = NULL;
static int  nhttpd_index_html_code = 0;
static char *monitord_ip = "127.0.0.1";
static int  monitord_port = 3082;
static void *argvmap = NULL;
static SESSION http_session = {0};
static int http_task_type = 0;
static int http_task_timeout = 10000000;
static int http_download_limit = 2097152;
#define E_OP_RESYNC             0x00
#define E_OP_STATE              0x01
static char *e_argvs[] =
{
    "op"
#define E_ARGV_OP           0
};
#define  E_ARGV_NUM         1
/* data handler */
int spider_packet_reader(CONN *conn, CB_DATA *buffer);
int spider_packet_handler(CONN *conn, CB_DATA *packet);
int spider_quick_handler(CONN *conn, CB_DATA *packet);
int spider_data_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int spider_error_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int spider_timeout_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int httpd_request_handler(CONN *conn, HTTP_REQ *http_req);
int spider_ok_handler(CONN *conn);
void spider_over_task();
/* httpd packet reader */
int httpd_packet_reader(CONN *conn, CB_DATA *buffer)
{
    if(conn)
    {
        return 0;
    }
    return -1;
}

/* converts hex char (0-9, A-Z, a-z) to decimal */
char hex2int(unsigned char hex)
{
    hex = hex - '0';
    if (hex > 9) {
        hex = (hex + '0' - 1) | 0x20;
        hex = hex - 'a' + 11;
    }
    if (hex > 15)
        hex = 0xFF;

    return hex;
}
#define URLDECODE(p, end, ps, high, low)                                                \
do                                                                                      \
{                                                                                       \
    while(*p != '\0' && *p != '&')                                                      \
    {                                                                                   \
        if(ps >= end) break;                                                            \
        else if(*p == '+') {*ps++ = 0x20;++p;continue;}                                 \
        else if(*p == '%')                                                              \
        {                                                                               \
            high = hex2int(*(p + 1));                                                   \
            if (high != 0xFF)                                                           \
            {                                                                           \
                low = hex2int(*(p + 2));                                                \
                if (low != 0xFF)                                                        \
                {                                                                       \
                    high = (high << 4) | low;                                           \
                    if (high < 32 || high == 127) high = '_';                           \
                    *ps++ = high;                                                       \
                    p += 3;                                                             \
                    continue;                                                           \
                }                                                                       \
            }                                                                           \
        }                                                                               \
        *ps++ = *p++;                                                                   \
    }                                                                                   \
    *ps = '\0';                                                                         \
}while(0)
/* data handler */
int http_packet_reader(CONN *conn, CB_DATA *buffer);
int http_packet_handler(CONN *conn, CB_DATA *packet);
int http_data_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int http_error_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int http_timeout_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk);
int http_trans_handler(CONN *conn, int tid);
int http_oob_handler(CONN *conn, CB_DATA *oob);
int http_download_redirect(int tid, int k, char *url);
int http_download_error(int tid, int k, int err);
int http_download_over(int tid, int k, int , HTTP_RESPONSE *resp, char *, size_t);

/* http download error */
int http_download_error(int tid, int k, int err)
{
    XTREC *rec = NULL;

    if(tasks[tid].conns[k].conn)
    {
        MUTEX_LOCK(tasks[tid].mutex);
        memset(tasks[tid].p, 0, sizeof(XTREC));
        rec = (XTREC *)tasks[tid].p;
        rec->id = tasks[tid].conns[k].meta.id;
        rec->err = err;
        tasks[tid].p += sizeof(XTREC) ;
        memset(&(tasks[tid].conns[k]), 0, sizeof(HTTPTASK));
        //fprintf(stdout, "%s::%d total:%d over:%d\r\n", __FILE__, __LINE__, tasks[tid].over, tasks[tid].total);
        if(++(tasks[tid].over) == tasks[tid].total) spider_over_task(tid);
        MUTEX_UNLOCK(tasks[tid].mutex);
    }
    return -1;
}

int http_packet_reader(CONN *conn, CB_DATA *buffer)
{
    if(conn)
    {
        return 0;
    }
    return -1;
}

/* download */
int http_packet_handler(CONN *conn, CB_DATA *packet)
{
    int n = 0, tid = 0, k = 0, mime = -1;
    HTTP_RESPONSE http_resp = {0};
    char *p = NULL, *end = NULL;

    if(conn)
    {
        tid = conn->session.xids[0];
        k = conn->session.xids[1];
        p = packet->data;
        end = packet->data + packet->ndata;
        /* http handler */        
        if(conn == tasks[tid].conns[k].conn)
        {
            if(p == NULL || http_response_parse(p, end, &http_resp, http_headers_map) == -1)
            {
                conn->over_cstate(conn);
                conn->over(conn);
                ERROR_LOGGER(logger, "Invalid http response header on TASK[%d][%d] via url:%s", 
                        tid, k, tasks[tid].conns[k].url);
                return http_download_error(tid, k, ERR_PROGRAM);
            }
            if(http_resp.respid >= 0 && http_resp.respid < HTTP_RESPONSE_NUM)
                p = response_status[http_resp.respid].e;
            DEBUG_LOGGER(logger, "Ready for handling http response:[%d:%s] on url[%s] "
                    "remote[%s:%d] local[%s:%d] via %d", http_resp.respid, p, 
                    tasks[tid].conns[k].url, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd);
            //location
            if(http_resp.respid == RESP_MOVEDPERMANENTLY
                && (n = http_resp.headers[HEAD_RESP_LOCATION]) > 0
                && (p = (http_resp.hlines + n)) )
            {
                return http_download_redirect(tid, k, p);
            }
            //check content-type
            if((n = http_resp.headers[HEAD_ENT_CONTENT_TYPE]) > 0 && (p = (http_resp.hlines + n)))
                mime = mime_id(&http_mime_map, p, strlen(p));
            if(http_resp.respid != RESP_OK
                || (http_mime_map.num > 0 && mime == -1))
            {
                conn->over_cstate(conn);
                conn->close(conn);
                ERROR_LOGGER(logger, "Invalid response [%s] on TASK[%d][%d].url:%s", 
                        http_resp.hlines, tid, k, tasks[tid].conns[k].url);
                return http_download_error(tid, k, ERR_CONTENT_TYPE);
            }
            else
            {
                conn->save_cache(conn, &http_resp, sizeof(HTTP_RESPONSE));
                if((n = http_resp.headers[HEAD_ENT_CONTENT_LENGTH]) > 0 
                        && (n = atol(http_resp.hlines + n)) > 0 
                        && n < http_download_limit)
                {
                    conn->recv_chunk(conn, n);
                }
                else
                {
                    conn->set_timeout(conn, HTTP_DOWNLOAD_TIMEOUT);
                    conn->recv_chunk(conn, HTML_MAX_SIZE);
                }
            }
        }
        else
        {
            FATAL_LOGGER(logger, "Invalid TASK[%d][%d] connection[%p][%s:%d] local[%s:%d] via %d,"
                    "url:%s", tid, k , conn, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd, tasks[tid].conns[k].url);
        }
    }
    return -1;
}

/* error handler */
int http_error_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    int tid = 0, k = 0;

    if(conn)
    {
        tid = conn->session.xids[0];
        k = conn->session.xids[1];
        if(tid >= 0 && k >= 0 && conn == tasks[tid].conns[k].conn && packet && cache && chunk) 
        {
            if(packet->ndata > 0 && cache->ndata > 0 && chunk->ndata > 0)
            {
                return http_data_handler(conn, packet, cache, chunk);
            }
            else
            {
                ERROR_LOGGER(logger, "download %s error_handler(%p) on remote[%s:%d] "
                    "local[%s:%d] via %d packet_len:%d cache_len:%d chunk_len:%d", 
                    tasks[tid].conns[k].url, conn, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd, packet->ndata, 
                    cache->ndata, chunk->ndata);
                conn->close(conn);
                return http_download_error(tid, k, ERR_TASK_CONN);
            }
        }
    }
    return -1;
}

/* timeout handler */
int http_timeout_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    int tid = 0, k = 0;

    if(conn)
    {
        tid = conn->session.xids[0];
        k = conn->session.xids[1];
        if(conn == tasks[tid].conns[k].conn && packet && cache && chunk) 
        {
            ERROR_LOGGER(logger, "TIMEOUT on %s:%d via %d, chunk->size:%d via url:%s", 
                        conn->remote_ip, conn->remote_port, conn->fd, 
                        chunk->ndata, tasks[tid].conns[k].url);
            if(packet->ndata > 0 && cache->ndata > 0 && chunk->ndata > 0)
            {
                return http_data_handler(conn, packet, cache, chunk);
            }
            else
            {
                conn->over_cstate(conn);
                conn->close(conn);
                return http_download_error(tid, k,  ERR_TASK_TIMEOUT);
            }
        }
    }
    return -1;
}

int http_download_redirect(int tid, int k, char *url)
{
    char *p = NULL, *s = NULL, *end = NULL, *ip = NULL, host[HTTP_HOST_MAX], 
         oldhost[HTTP_HOST_MAX];
    int ret = -1, port = 0, oldport = 0, nurl = 0;
    struct hostent *hp = NULL;
    CB_DATA *chunk = NULL;
    SESSION sess = {0};
    CONN *conn = NULL;

    if(tid >= 0 && k >= 0 && (conn = tasks[tid].conns[k].conn) && (p = url) 
            && (nurl = strlen(url)) > 0 && (end = (url + nurl)) > url)
    {
        if((p = strstr(url, "http://")))
        {
            p += 7;
            s = host;
            while(p < end && *p != '/' && *p != ':')
            {
                if(*p >= 'A' && *p <= 'Z')
                    *s++ = *p + ('a' - 'A');
                else 
                    *s++ = *p++;
            }
            *s = '\0';
            if(*p == ':') port = atoi(++p);
        }
        if((p = strstr(tasks[tid].conns[k].url, "http://")))
        {
            p += 7;
            s = oldhost;
            while(p < end && *p != '/' && *p != ':')
            {
                if(*p >= 'A' && *p <= 'Z')
                    *s++ = *p + ('a' - 'A');
                else 
                    *s++ = *p++;
            }
            *s = '\0';
            if(*p == ':') oldport = atoi(++p);
        }
        strcpy(tasks[tid].conns[k].url, url);
        if(tasks[tid].conns[k].meta.proxy_ip == 0  
            && (strcmp(host, oldhost) != 0 || port != oldport))
        {
            conn->close(conn);
            tasks[tid].conns[k].conn = NULL;
            if((hp = gethostbyname(host)))
            {
                ip = host;
                sprintf(ip, "%s", inet_ntoa(*((struct in_addr *)(hp->h_addr))));
                tasks[tid].conns[k].meta.ip = inet_addr(ip);
                if(port == 0) port = 80;
                memcpy(&sess, &http_session, sizeof(SESSION)); 
                sess.xids[0] = tid;
                sess.xids[1] = k;
                if((tasks[tid].conns[k].conn = httpd->newconn(httpd, 
                                -1, -1, ip, port, &sess)) == NULL)
                {
                    return http_download_error(tid, k, ERR_NETWORK);
                }
            }
            else 
                return http_download_error(tid, k, ERR_NETWORK);
        }
        else
        {
            if((chunk = conn->newchunk(conn, HTTP_HEADER_MAX)))
            {
                p = chunk->data;
                p += sprintf(p, "GET %s HTTP/1.1\r\n%s\r\n", tasks[tid].conns[k].url, 
                        tasks[tid].conns[k].headers);
                if(conn->send_chunk(conn, chunk, (p - chunk->data)) != 0) 
                {
                    conn->freechunk(conn, chunk);
                    return http_download_error(tid, k, ERR_NETWORK);
                }
            }
        }
    }
    return ret;
}
/* download over */
int http_download_over(int tid, int k, int is_need_compress, 
        HTTP_RESPONSE *http_resp, char *rawdata, size_t nrawdata)
{
    char *p = NULL, *ps = NULL, *zdata = NULL;
    int ret = -1, i = 0, n = 0;
    size_t nzdata = 0;
    XTREC *rec = NULL;

    if(http_resp && rawdata && nrawdata > 0)
    {
        MUTEX_LOCK(tasks[tid].mutex);
        p = tasks[tid].p;
        memset(p, 0, sizeof(XTREC));
        rec = (XTREC *)p;
        rec->id = tasks[tid].conns[k].meta.id;
        p += sizeof(XTREC);
        /* location */
        if(tasks[tid].conns[k].flag & HTTP_REDIRECT)
        {
            rec->nlocation = sprintf(p, "%s", tasks[tid].conns[k].url);
            p += rec->nlocation + 1;
        }
        /* last modified */
        if((n = http_resp->headers[HEAD_ENT_LAST_MODIFIED]))
        {
            rec->last_modified = str2time(http_resp->hlines + n);
        }
        /* cookie */
        if(http_resp->ncookies > 0)
        {
            ps = p;
            i = 0;
            do
            {
                p += sprintf(p, "%.*s=%.*s; ", http_resp->cookies[i].nk, 
                        http_resp->hlines + http_resp->cookies[i].k,
                        http_resp->cookies[i].nv, http_resp->hlines 
                        + http_resp->cookies[i].v);
            }while(++i < http_resp->ncookies);
            rec->ncookie = p - ps;
            *p++ = '\0';
        }
        /* compress with zlib::inflate() */
        DEBUG_LOGGER(logger, "url:%s is_need_compess:%d data:%p ndata:%u", tasks[tid].conns[k].url, is_need_compress, rawdata, LI(nrawdata));
        ps = p;
        if(is_need_compress)
        {
            zdata = p;
            nzdata = nrawdata + Z_HEADER_SIZE;
            if(zcompress((Bytef *)rawdata, nrawdata, (Bytef *)zdata, (uLong * )((void *)&nzdata)) == 0)
            {
                p += nzdata;
            }
            DEBUG_LOGGER(logger, "compressed %s data %u to %u via urlid:%d", tasks[tid].conns[k].url, LI(nrawdata), LI(nzdata), tasks[tid].conns[k].id);
        }
        else
        {
            memcpy(p, rawdata, nrawdata);
            p += nrawdata;
        }
        rec->length = p - ps; 
        DEBUG_LOGGER(logger, "url:%s id:%d nlocation:%d length:%d/%d ncookie:%d", tasks[tid].conns[k].url, tasks[tid].conns[k].id, rec->nlocation, rec->length, p - ps, rec->ncookie);
        tasks[tid].p = p;
        memset(&(tasks[tid].conns[k]), 0, sizeof(HTTPTASK));
        if(++(tasks[tid].over) == tasks[tid].total) spider_over_task(tid);
        MUTEX_UNLOCK(tasks[tid].mutex);
    }
    return ret;
}

/* data handler */
int http_data_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    size_t ninbuf = 0, noutbuf = 0, nzdata = 0, ndata = 0, nrawdata = 0, m = 0;
    char charset[CHARSET_MAX], *zdata = NULL, *p = NULL, *ps = NULL, *xmm = NULL,
         *s = NULL, *outbuf = NULL, *data = NULL, *rawdata = NULL, *mm = NULL, *block = NULL;
    int  ret = -1, tid = 0, k = 0, i = 0, n = 0, is_need_convert = 0, 
         is_need_compress = 0, is_text = 0;
    HTTP_RESPONSE *http_resp = NULL;
    HTTP_CHUNK http_chunk;
    chardet_t pdet = NULL;
    iconv_t cd = NULL;

    if(conn)
    {
        tid = conn->session.xids[0];
        k = conn->session.xids[1];
        if(conn == tasks[tid].conns[k].conn && chunk && chunk->data && chunk->ndata > 0
                && cache && cache->ndata > 0 && (http_resp = (HTTP_RESPONSE *)cache->data))
        {
            //doc_total++;
            /* check content is text */
            if(http_task_type != XT_TASK_FILE || ((n = http_resp->headers[HEAD_ENT_CONTENT_TYPE]) > 0
                && (p = (http_resp->hlines + n))  && (strcasestr(p, "text") 
                    || strcasestr(p, "html") || strcasestr(p, "plain"))))
            {
                is_need_compress = 1;
                is_text = 1;
            }
            memset(&http_chunk, 0, sizeof(HTTP_CHUNK));
            if((n = http_resp->headers[HEAD_GEN_TRANSFER_ENCODING]) > 0 
                    && (p = (http_resp->hlines + n))  && strcasestr(p, "chunked")
                    && (n = http_chunked_parse(&http_chunk, chunk->data, chunk->ndata)) > 0)
            {
                s = xmm = xmm_new(HTTP_BLOCK_MAX);
                for(i = 0; i < n; i++)
                {
                    memcpy(s, chunk->data + http_chunk.chunks[i].off, http_chunk.chunks[i].len);
                    s += http_chunk.chunks[i].len;
                }
                rawdata = xmm;
                nrawdata = s - xmm;
            }
            else
            {
                rawdata = chunk->data;
                nrawdata = chunk->ndata;
            }
            /* check content encoding  */
            if(is_text && (n = http_resp->headers[HEAD_ENT_CONTENT_ENCODING]) > 0 
                    && (p = (http_resp->hlines + n)) && (mm = (char *)xmm_new(HTTP_BLOCK_MAX)))
            {
                zdata = rawdata;//chunk->data;
                nzdata = nrawdata;//(size_t)chunk->ndata;
                data = mm;
                ndata = HTTP_BLOCK_MAX;
                if(strcasestr(p, "gzip")) 
                {
                    if((n = httpgzdecompress((Bytef *)zdata, nzdata, (Bytef *)data, (uLong *)((void *)&ndata))) == 0)
                    {
                        rawdata = data;
                        nrawdata = ndata;
                        is_need_compress = 1;
                    }
                    else 
                    {
                        WARN_LOGGER(logger, "httpgzdecompress() => %d url:%s data from %u to %u failed, %s", n, tasks[tid].conns[k].url, LI(nzdata), LI(ndata), strerror(errno));
                        goto err_end;
                    }
                }
                else if(strcasestr(p, "deflate"))
                {
                    if(zdecompress((Bytef *)zdata, nzdata, (Bytef *)data, 
                                (uLong *)((void *)&ndata)) == 0)
                    {
                        rawdata = data;
                        nrawdata = ndata;
                    }
                    else 
                    {
                        WARN_LOGGER(logger, "gzdecompress url:%s data from %u to %u failed, %s", 
                            tasks[tid].conns[k].url, LI(nzdata), LI(ndata), strerror(errno));
                        goto err_end;
                    }
                }
                else
                {
                    WARN_LOGGER(logger, "unspported url:%s data encoding:%s", p, tasks[tid].conns[k].url);
                    goto err_end;
                }
            }
            /* check text/plain/html/xml...  charset  */
            memset(charset, 0, CHARSET_MAX);
            if(is_text)
            {
                DEBUG_LOGGER(logger, "%s is_need_convert:%d data:%p ndata:%u via urlid:%d", tasks[tid].conns[k].url , is_need_convert, rawdata, LI(nrawdata), tasks[tid].conns[k].id);
                if(rawdata && nrawdata > 0 && chardet_create(&pdet) == 0)
                {
                    if(chardet_handle_data(pdet, rawdata, nrawdata) == 0 
                            && chardet_data_end(pdet) == 0 )
                    {
                        chardet_get_charset(pdet, charset, CHARSET_MAX);
                        if(memcmp(charset, "UTF-8", 5) != 0) is_need_convert = 1;
                    }
                    chardet_destroy(pdet);
                }
                DEBUG_LOGGER(logger, "url:%s is_need_convert:%d data:%p ndata:%u", tasks[tid].conns[k].url, is_need_convert, rawdata, LI(nrawdata));
                /* convert charset  */
                if(is_need_convert && (cd = iconv_open("UTF-8", charset)) != (iconv_t)-1
                        && (block = (char *)xmm_new(HTTP_BLOCK_MAX)))
                {
                    p = rawdata;
                    ninbuf = nrawdata;
                    m = noutbuf = HTTP_BLOCK_MAX;
                    ps = outbuf = block;
                    if(iconv(cd, &p, &ninbuf, &ps, &m) == (size_t)-1)
                    {
                        WARN_LOGGER(logger, "convert url:%s data from %s UTF-8 failed, %s",tasks[tid].conns[k].url, charset, strerror(errno));
                        outbuf = NULL;
                    }
                    else
                    {
                        noutbuf -= m;
                        DEBUG_LOGGER(logger, "convert url:%s %s[%u] to UTF-8 len:%u", tasks[tid].conns[k].url, charset, LI(nrawdata), LI(noutbuf));
                        rawdata = outbuf;
                        nrawdata = noutbuf;
                    }
                    iconv_close(cd);
                    if(is_need_compress == 0) is_need_compress = 1;
                }
            }
            rawdata[nrawdata] = 0;
            //fprintf(stdout, "%s::%d nrawdata:%d\r\n", __FILE__, __LINE__, nrawdata);
            ret = http_download_over(tid, k, is_need_compress, http_resp, rawdata, nrawdata);
        }
err_end:
        if(ret == -1) http_download_error(tid, k, ERR_DATA);
        conn->close(conn);
        if(block) xmm_free(block, HTTP_BLOCK_MAX);
        if(xmm) xmm_free(xmm, HTTP_BLOCK_MAX);
        if(mm) xmm_free(mm, HTTP_BLOCK_MAX);
    }
    return ret;
}

/* http ok handler */
int http_ok_handler(CONN *conn)
{
    CB_DATA *chunk = NULL;
    int tid = 0, k = 0;
    char *p = NULL;

    if(conn)
    {
        tid = conn->session.xids[0];
        k = conn->session.xids[1];
        if((chunk = conn->newchunk(conn, HTTP_HEADER_MAX)))
        {
            p = chunk->data;
            p += sprintf(p, "GET %s HTTP/1.1\r\n%s\r\n", tasks[tid].conns[k].url, 
                    tasks[tid].conns[k].headers);
            //fprintf(stdout, "%s::%d header:%s\r\n", __FILE__, __LINE__, chunk->data);
            if(conn->send_chunk(conn, chunk, p - chunk->data) != 0) 
            {
                conn->freechunk(conn, chunk);
                return http_download_error(tid, k, ERR_NETWORK);
            }
        }
        return 0;
    }
    return -1;
}

/* httpd request handler */
int httpd_request_handler(CONN *conn, HTTP_REQ *http_req)
{
    char *p = NULL, buf[HTTP_BUF_SIZE], line[HTTP_LINE_MAX];
    int id = -1, n = -1, op = -1, i = 0;

    if(conn && http_req)
    {
        for(i = 0; i < http_req->nargvs; i++)
        {

            if(http_req->argvs[i].nk > 0 && (n = http_req->argvs[i].k) > 0
                    && (p = (http_req->line + n)))
            {
                 if((id = (mtrie_get(argvmap, p, http_req->argvs[i].nk) - 1)) >= 0
                        && http_req->argvs[i].nv > 0
                        && (n = http_req->argvs[i].v) > 0
                        && (p = (http_req->line + n)))
                {
                    switch(id)
                    {
                        case E_ARGV_OP :
                            op = atoi(p);
                            break;
                        default :
                            break;
                    }
                }
            }
        }
        if(op == E_OP_STATE)
        {
            //if((n = kindex_state(kindex, line)) > 0)        
            {
                p = buf;
                p += sprintf(p, "HTTP/1.0 200 OK\r\nContent-Length: %d\r\n"
                        "Content-Type: text/html;charset=%s\r\n\r\n",n, http_default_charset);
                conn->push_chunk(conn, buf, (p - buf));
                conn->push_chunk(conn, line, n);
                return conn->over(conn);
            }
        }
    }
    return -1;
}

/* packet handler */
int httpd_packet_handler(CONN *conn, CB_DATA *packet)
{
    char buf[HTTP_BUF_SIZE], file[HTTP_PATH_MAX], *p = NULL, *end = NULL;
    HTTP_REQ http_req = {0};
    struct stat st = {0};
    int ret = -1, n = 0;

    if(conn && packet)
    {
        p = packet->data;
        end = packet->data + packet->ndata;
        if(http_request_parse(p, end, &http_req, http_headers_map) == -1) goto err_end;
        if(http_req.reqid == HTTP_GET)
        {
            if(http_req.nargvs > 0)
            {
                if(httpd_request_handler(conn, &http_req) < 0) goto err_end;
            }
            else if(is_inside_html && httpd_index_html_code && nhttpd_index_html_code > 0)
            {
                p = buf;
                p += sprintf(p, "HTTP/1.0 200 OK\r\nContent-Length: %d\r\n"
                        "Content-Type: text/html;charset=%s\r\n",
                        nhttpd_index_html_code, http_default_charset);
                if((n = http_req.headers[HEAD_GEN_CONNECTION]) > 0)
                    p += sprintf(p, "Connection: %s\r\n", (http_req.hlines + n));
                p += sprintf(p, "Connection:Keep-Alive\r\n");
                p += sprintf(p, "\r\n");
                conn->push_chunk(conn, buf, (p - buf));
                conn->push_chunk(conn, httpd_index_html_code, nhttpd_index_html_code);
                return conn->over(conn);
            }
            else if(httpd_home)
            {
                p = file;
                if(http_req.path[0] != '/')
                    p += sprintf(p, "%s/%s", httpd_home, http_req.path);
                else
                    p += sprintf(p, "%s%s", httpd_home, http_req.path);
                if(http_req.path[1] == '\0')
                    p += sprintf(p, "%s", "index.html");
                if((n = (p - file)) > 0 && stat(file, &st) == 0)
                {
                    if(st.st_size == 0)
                    {
                        conn->push_chunk(conn, HTTP_NO_CONTENT, strlen(HTTP_NO_CONTENT));
                        return conn->over(conn);
                    }
                    else if((n = http_req.headers[HEAD_REQ_IF_MODIFIED_SINCE]) > 0
                            && str2time(http_req.hlines + n) == st.st_mtime)
                    {
                        conn->push_chunk(conn, HTTP_NOT_MODIFIED, strlen(HTTP_NOT_MODIFIED));
                        return conn->over(conn);
                    }
                    else
                    {
                        p = buf;
                        p += sprintf(p, "HTTP/1.0 200 OK\r\nContent-Length:%lld\r\n"
                                "Content-Type: text/html;charset=%s\r\n",
                                (long long int)(st.st_size), http_default_charset);
                        if((n = http_req.headers[HEAD_GEN_CONNECTION]) > 0)
                            p += sprintf(p, "Connection: %s\r\n", http_req.hlines + n);
                        p += sprintf(p, "Last-Modified:");
                        p += GMTstrdate(st.st_mtime, p);
                        p += sprintf(p, "%s", "\r\n");//date end
                        p += sprintf(p, "%s", "\r\n");
                        conn->push_chunk(conn, buf, (p - buf));
                        conn->push_file(conn, file, 0, st.st_size);
                        return conn->over(conn);
                    }
                }
                else
                {
                    conn->push_chunk(conn, HTTP_NOT_FOUND, strlen(HTTP_NOT_FOUND));
                    return conn->over(conn);
                }
            }
        }
        else if(http_req.reqid == HTTP_POST)
        {
            if((n = http_req.headers[HEAD_ENT_CONTENT_LENGTH]) > 0
                    && (n = atol(http_req.hlines + n)) > 0)
            {
                conn->save_cache(conn, &http_req, sizeof(HTTP_REQ));
                return conn->recv_chunk(conn, n);
            }
        }
err_end:
        ret = conn->push_chunk(conn, HTTP_BAD_REQUEST, strlen(HTTP_BAD_REQUEST));
        return conn->over(conn);
    }
    return ret;
}

/*  data handler */
int httpd_data_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    int ret = -1;

    if(conn && packet && cache && chunk && chunk->ndata > 0)
    {
        ret = 0;
    }
    return ret;
}

/* httpd timeout handler */
int httpd_timeout_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    if(conn)
    {
        conn->over_timeout(conn);
        return conn->over(conn);
    }
    return -1;
}

/* OOB data handler for httpd */
int httpd_oob_handler(CONN *conn, CB_DATA *oob)
{
    if(conn)
    {
        return 0;
    }
    return -1;
}


/* packet reader */
int spider_packet_reader(CONN *conn, CB_DATA *buffer)
{
    if(conn) return 0;
    return -1;
}

/* packet handler */
int spider_packet_handler(CONN *conn, CB_DATA *packet)
{
    if(conn && packet)
    {
        conn->wait_estate(conn);
        conn->set_timeout(conn, TASK_WAIT_TIME);
        return 0;
    }
    return -1;
}

/* packet quick handler */
int spider_quick_handler(CONN *conn, CB_DATA *packet)
{
    XTHEAD *head = NULL;
    if(conn && (head = (XTHEAD *)(packet->data)))
    {
        return head->length;
    }
    return -1;
}

/* data handler */
int spider_data_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    char *p = NULL, *end = NULL, ip[HTTP_IP_MAX];
    int n = 0, tid = 0, k = 0, port = 0;
    unsigned char *s = NULL;
    XTMETA *meta = NULL;
    SESSION sess = {0};


    if(conn)
    {
        if((p = chunk->data) && (end = (p + chunk->ndata)) > p 
                && (tid = (conn->session.xids[0] - 1)) >= 0)
        {
            memcpy(&sess, &http_session, sizeof(SESSION));
            memset(&(tasks[tid].conns), 0, sizeof(HTTPTASK) * HTTP_TASK_MAX);
            tasks[tid].total = tasks[tid].over = 0;
            tasks[tid].conn = conn;
            tasks[tid].status = HTTP_TASK_INIT;
            tasks[tid].chunk = conn->newchunk(conn, HTTP_BLOCK_MAX);
            tasks[tid].p = tasks[tid].chunk->data + sizeof(XTHEAD);
            k = 0;
            while(p < end)
            {
                memcpy(&(tasks[tid].conns[k].meta), p, sizeof(XTMETA)); 
                meta = (XTMETA *)p; 
                tasks[tid].conns[k].id = meta->id;
                p += sizeof(XTMETA);
                strncpy(tasks[tid].conns[k].url, p, meta->nurl);
                p += meta->nurl;
                strncpy(tasks[tid].conns[k].headers, p, meta->nheaders);
                p += meta->nheaders;
                if(meta->proxy_ip && (port = meta->proxy_port))
                {
                    s = (unsigned char *)&(meta->proxy_ip);
                    n = sprintf(ip, "%d.%d.%d.%d", s[0], s[1], s[2], s[3]);
                }
                else
                {
                    s = (unsigned char *)&(meta->ip);
                    n = sprintf(ip, "%d.%d.%d.%d", s[0], s[1], s[2], s[3]);  
                        port = HTTP_DEFAULT_PORT;
                    if(meta->port) port = meta->port;
                }
                //fprintf(stdout, "%s::%d %s:%d header:%s\r\n", __FILE__, __LINE__, ip, port, tasks[tid].conns[k].headers);
                sess.xids[0] = tid;
                sess.xids[1] = k;
                if((tasks[tid].conns[k].conn = httpd->newconn(httpd, 
                                -1, -1, ip, port, &sess)))
                {
                    ++k;
                }
            }
            MUTEX_LOCK(tasks[tid].mutex);
            tasks[tid].total = k;
            if(tasks[tid].over == k) spider_over_task(tid);
            MUTEX_UNLOCK(tasks[tid].mutex);
            return 0;
        }
    }
    return -1;
}

/* over task */
void spider_over_task(int tid)
{
    CB_DATA *chunk = NULL;
    XTHEAD *head = NULL;
    CONN *conn = NULL;
    int n = 0;

    if(tid >= 0 && (conn = tasks[tid].conn) && (chunk = tasks[tid].chunk) 
            && (head = (XTHEAD *)chunk->data))
    {
        n = tasks[tid].p - tasks[tid].chunk->data;
        head->cmd = XT_REQ_DOWNLOAD;
        head->flag |= (http_task_type|XT_TASK_OVER);
        head->length = n - sizeof(XTHEAD);
        tasks[tid].chunk = NULL;
        if(conn->send_chunk(conn, chunk, n) != 0)
        {
            conn->freechunk(conn, chunk);
            conn->close(conn);
            iqueue_push(taskqueue, tid);
        }
    }
    return ;
}

/* ok handler */
int spider_ok_handler(CONN *conn)
{
    XTHEAD head = {0};
    if(conn)
    {
       head.cmd = XT_REQ_DOWNLOAD; 
       head.flag = http_task_type; 
        return conn->push_chunk(conn, &head, sizeof(XTHEAD));
    }
    return -1;
}

/* error handler */
int spider_error_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    int tid = 0;

    if(conn && (tid = conn->session.xids[0] - 1) >= 0)
    {
        iqueue_push(taskqueue, tid);
        return 0;
    }
    return -1;
}

/* timeout handler */
int spider_timeout_handler(CONN *conn, CB_DATA *packet, CB_DATA *cache, CB_DATA *chunk)
{
    if(conn)
    {
        conn->over_estate(conn);
        conn->over_timeout(conn);
        return spider_ok_handler(conn);
    }
    return -1;
}

/* OOB handler */
int spider_oob_handler(CONN *conn, CB_DATA *oob)
{
    if(conn)
    {
        return 0;
    }
    return -1;
}

/* heartbeat */
void cb_heartbeat_handler(void *arg)
{
    int i = 0, id = 0, total = 0, is_add_to_queue = 0;
    CONN *conn = NULL;
    SESSION sess = {0};

    if(arg == (void *)spider)
    {
        memcpy(&sess, &(spider->session), sizeof(SESSION));
        total = QTOTAL(taskqueue);
        for(i = 0; i < total; i++)
        {
            id = -1;
            is_add_to_queue = 0;
            iqueue_pop(taskqueue, &id);
            if(id >= 0 && id < ntasks)
            {
                if(tasks[id].status == 0)
                {
                    sess.xids[0] = id+1;
                    if((conn = spider->newconn(spider, -1,-1,
                                    monitord_ip,monitord_port, &sess)))
                    {
                        tasks[id].status = 1;
                    }
                    else 
                    {
                        is_add_to_queue = 1;
                    }
                }
                else is_add_to_queue = 1;
                if(is_add_to_queue)
                {
                    tasks[id].status = 0;
                    iqueue_push(taskqueue, id);
                }
            }
        }
    }
    return ;
}

/* Initialize from ini file */
int sbase_initialize(SBASE *sbase, char *conf)
{
    char *s = NULL, *p = NULL, *end = NULL;// *dictfile = NULL, *dictrules = NULL, *host = NULL; 
         //*property_name = NULL, *text_index_name = NULL, *int_index_name = NULL,
         //*long_index_name = NULL, *double_index_name = NULL, *display_name = NULL,
         //*block_name = NULL, *key_name;
    int interval = 0, i = 0, n = 0;// port = 0; //commitid = 0, queueid = 0;

    if((dict = iniparser_new(conf)) == NULL)
    {
        fprintf(stderr, "Initializing conf:%s failed, %s\n", conf, strerror(errno));
        _exit(-1);
    }
    /* http headers map */
    if((http_headers_map = http_headers_map_init()) == NULL)
    {
        fprintf(stderr, "Initialize http_headers_map failed,%s", strerror(errno));
        _exit(-1);
    }
    /* SBASE */
    sbase->nchilds = iniparser_getint(dict, "SBASE:nchilds", 0);
    sbase->connections_limit = iniparser_getint(dict, "SBASE:connections_limit", SB_CONN_MAX);
    setrlimiter("RLIMIT_NOFILE", RLIMIT_NOFILE, sbase->connections_limit);
    sbase->usec_sleep = iniparser_getint(dict, "SBASE:usec_sleep", SB_USEC_SLEEP);
    sbase->set_log(sbase, iniparser_getstr(dict, "SBASE:logfile"));
    sbase->set_log_level(sbase, iniparser_getint(dict, "SBASE:log_level", 0));
    sbase->set_evlog(sbase, iniparser_getstr(dict, "SBASE:evlogfile"));
    sbase->set_evlog_level(sbase, iniparser_getint(dict, "SBASE:evlog_level", 0));
    if((argvmap = mtrie_init()) == NULL) _exit(-1);
    else
    {
        for(i = 0; i < E_ARGV_NUM; i++)
        {
            mtrie_add(argvmap, e_argvs[i], strlen(e_argvs[i]), i+1);
        }
    }
    /* httpd */
    is_inside_html = iniparser_getint(dict, "HTTPD:is_inside_html", 1);
    httpd_home = iniparser_getstr(dict, "HTTPD:httpd_home");
    http_default_charset = iniparser_getstr(dict, "HTTPD:httpd_charset");
    /* decode html base64 */
    /*
    if(html_code_base64 && (n = strlen(html_code_base64)) > 0
            && (httpd_index_html_code = (unsigned char *)calloc(1, n + 1)))
    {
        nhttpd_index_html_code = base64_decode(httpd_index_html_code,
                (char *)html_code_base64, n);
    }
    */
    if((httpd = service_init()) == NULL)
    {
        fprintf(stderr, "Initialize service failed, %s", strerror(errno));
        _exit(-1);
    }
    httpd->family = iniparser_getint(dict, "HTTPD:inet_family", AF_INET);
    httpd->sock_type = iniparser_getint(dict, "HTTPD:socket_type", SOCK_STREAM);
    httpd->ip = iniparser_getstr(dict, "HTTPD:service_ip");
    httpd->port = iniparser_getint(dict, "HTTPD:service_port", 80);
    httpd->working_mode = iniparser_getint(dict, "HTTPD:working_mode", WORKING_PROC);
    httpd->service_type = iniparser_getint(dict, "HTTPD:service_type", S_SERVICE);
    httpd->service_name = iniparser_getstr(dict, "HTTPD:service_name");
    httpd->nprocthreads = iniparser_getint(dict, "HTTPD:nprocthreads", 1);
    httpd->ndaemons = iniparser_getint(dict, "HTTPD:ndaemons", 0);
    httpd->niodaemons = iniparser_getint(dict, "HTTPD:niodaemons", 1);
    httpd->use_cond_wait = iniparser_getint(dict, "HTTPD:use_cond_wait", 0);
    if(iniparser_getint(dict, "HTTPD:use_cpu_set", 0) > 0) httpd->flag |= SB_CPU_SET;
    if(iniparser_getint(dict, "HTTPD:event_lock", 0) > 0) httpd->flag |= SB_EVENT_LOCK;
    if(iniparser_getint(dict, "HTTPD:newconn_delay", 0) > 0) httpd->flag |= SB_NEWCONN_DELAY;
    if(iniparser_getint(dict, "HTTPD:tcp_nodelay", 0) > 0) httpd->flag |= SB_TCP_NODELAY;
    if(iniparser_getint(dict, "HTTPD:socket_linger", 0) > 0) httpd->flag |= SB_SO_LINGER;
    if(iniparser_getint(dict, "HTTPD:while_send", 0) > 0) httpd->flag |= SB_WHILE_SEND;
    if(iniparser_getint(dict, "HTTPD:log_thread", 0) > 0) httpd->flag |= SB_LOG_THREAD;
    if(iniparser_getint(dict, "HTTPD:use_outdaemon", 0) > 0) httpd->flag |= SB_USE_OUTDAEMON;
    if(iniparser_getint(dict, "HTTPD:use_evsig", 0) > 0) httpd->flag |= SB_USE_EVSIG;
    if(iniparser_getint(dict, "HTTPD:use_cond", 0) > 0) httpd->flag |= SB_USE_COND;
    if((n = iniparser_getint(dict, "HTTPD:sched_realtime", 0)) > 0) httpd->flag |= (n & (SB_SCHED_RR|SB_SCHED_FIFO));
    if((n = iniparser_getint(dict, "HTTPD:io_sleep", 0)) > 0) httpd->flag |= ((SB_IO_NANOSLEEP|SB_IO_USLEEP|SB_IO_SELECT) & n);
    httpd->nworking_tosleep = iniparser_getint(dict, "HTTPD:nworking_tosleep", SB_NWORKING_TOSLEEP);
    httpd->set_log(httpd, iniparser_getstr(dict, "HTTPD:logfile"));
    httpd->set_log_level(httpd, iniparser_getint(dict, "HTTPD:log_level", 0));
    httpd->session.packet_type = iniparser_getint(dict, "HTTPD:packet_type",PACKET_DELIMITER);
    httpd->session.packet_delimiter = iniparser_getstr(dict, "HTTPD:packet_delimiter");
    p = s = httpd->session.packet_delimiter;
    while(*p != 0 )
    {
        if(*p == '\\' && *(p+1) == 'n')
        {
            *s++ = '\n';
            p += 2;
        }
        else if (*p == '\\' && *(p+1) == 'r')
        {
            *s++ = '\r';
            p += 2;
        }
        else
            *s++ = *p++;
    }
    *s++ = 0;
    httpd->session.packet_delimiter_length = strlen(httpd->session.packet_delimiter);
    httpd->session.buffer_size = iniparser_getint(dict, "HTTPD:buffer_size", SB_BUF_SIZE);
    httpd->session.packet_reader = &httpd_packet_reader;
    httpd->session.packet_handler = &httpd_packet_handler;
    httpd->session.data_handler = &httpd_data_handler;
    httpd->session.timeout_handler = &httpd_timeout_handler;
    httpd->session.oob_handler = &httpd_oob_handler;
    /* http request handler */
    memcpy(&http_session, &(httpd->session), sizeof(SESSION));
    http_session.timeout = http_task_timeout; 
    http_session.packet_reader = &http_packet_reader;
    http_session.packet_handler = &http_packet_handler;
    http_session.data_handler = &http_data_handler;
    http_session.ok_handler = &http_ok_handler;
    http_session.timeout_handler = &http_timeout_handler;
    http_session.error_handler = &http_error_handler;
    http_session.flags = SB_NONBLOCK;
    /* http mime map */
    if((p = iniparser_getstr(dict, "HTTPD:mime")))
    {
        end = p + strlen(p);
        if((mime_map_init(&http_mime_map)) != 0)
        {
            fprintf(stderr, "Initialize http_mime_map failed,%s", strerror(errno));
            _exit(-1);
        }
        mime_add_line(&http_mime_map, p, end);
    }
    /* SPIDER */
    if((spider = service_init()) == NULL)
    {
        fprintf(stderr, "Initialize service failed, %s", strerror(errno));
        _exit(-1);
    }
    spider->family = iniparser_getint(dict, "SPIDER:inet_family", AF_INET);
    spider->sock_type = iniparser_getint(dict, "SPIDER:socket_type", SOCK_STREAM);
    monitord_ip = spider->ip = iniparser_getstr(dict, "SPIDER:service_ip");
    monitord_port = spider->port = iniparser_getint(dict, "SPIDER:service_port", 3082);
    spider->working_mode = iniparser_getint(dict, "SPIDER:working_mode", WORKING_PROC);
    spider->service_type = iniparser_getint(dict, "SPIDER:service_type", C_SERVICE);
    spider->service_name = iniparser_getstr(dict, "SPIDER:service_name");
    spider->nprocthreads = iniparser_getint(dict, "SPIDER:nprocthreads", 1);
    spider->niodaemons = iniparser_getint(dict, "SPIDER:niodaemons", 1);
    spider->use_cond_wait = iniparser_getint(dict, "SPIDER:use_cond_wait", 0);
    if(iniparser_getint(dict, "SPIDER:use_cpu_set", 0) > 0) spider->flag |= SB_CPU_SET;
    if(iniparser_getint(dict, "SPIDER:event_lock", 0) > 0) spider->flag |= SB_EVENT_LOCK;
    if(iniparser_getint(dict, "SPIDER:newconn_delay", 0) > 0) spider->flag |= SB_NEWCONN_DELAY;
    if(iniparser_getint(dict, "SPIDER:tcp_nodelay", 0) > 0) spider->flag |= SB_TCP_NODELAY;
    if(iniparser_getint(dict, "SPIDER:socket_linger", 0) > 0) spider->flag |= SB_SO_LINGER;
    if(iniparser_getint(dict, "SPIDER:while_send", 0) > 0) spider->flag |= SB_WHILE_SEND;
    if(iniparser_getint(dict, "SPIDER:log_thread", 0) > 0) spider->flag |= SB_LOG_THREAD;
    if(iniparser_getint(dict, "SPIDER:use_outdaemon", 0) > 0) spider->flag |= SB_USE_OUTDAEMON;
    if(iniparser_getint(dict, "SPIDER:use_evsig", 0) > 0) spider->flag |= SB_USE_EVSIG;
    if(iniparser_getint(dict, "SPIDER:use_cond", 0) > 0) spider->flag |= SB_USE_COND;
    if((n = iniparser_getint(dict, "SPIDER:sched_realtime", 0)) > 0) spider->flag |= (n & (SB_SCHED_RR|SB_SCHED_FIFO));
    if((n = iniparser_getint(dict, "SPIDER:io_sleep", 0)) > 0) spider->flag |= ((SB_IO_NANOSLEEP|SB_IO_USLEEP|SB_IO_SELECT) & n);
    spider->nworking_tosleep = iniparser_getint(dict, "SPIDER:nworking_tosleep", SB_NWORKING_TOSLEEP);
    spider->set_log(spider, iniparser_getstr(dict, "SPIDER:logfile"));
    spider->set_log_level(spider, iniparser_getint(dict, "SPIDER:log_level", 0));
    spider->session.packet_type = PACKET_CERTAIN_LENGTH;
    spider->session.packet_length = sizeof(XTHEAD);
    spider->session.buffer_size = iniparser_getint(dict, "SPIDER:buffer_size", SB_BUF_SIZE);
    spider->session.packet_reader = &spider_packet_reader;
    spider->session.packet_handler = &spider_packet_handler;
    spider->session.quick_handler = &spider_quick_handler;
    spider->session.ok_handler = &spider_ok_handler;
    spider->session.data_handler = &spider_data_handler;
    spider->session.error_handler = &spider_error_handler;
    spider->session.timeout_handler = &spider_timeout_handler;
    spider->session.oob_handler = &spider_oob_handler;
    spider->session.flags = SB_NONBLOCK;
    interval = iniparser_getint(dict, "SPIDER:heartbeat_interval", SB_HEARTBEAT_INTERVAL);
    spider->set_heartbeat(spider, interval, &cb_heartbeat_handler, spider);
    LOGGER_INIT(logger, iniparser_getstr(dict, "SPIDER:access_log"));
    ntasks = iniparser_getint(dict, "SPIDER:ntasks", 64);
    http_task_type = iniparser_getint(dict, "SPIDER:task_type", 0);
    http_task_timeout = iniparser_getint(dict, "SPIDER:task_timeout", 10000000);
    tasks = (WTASK *)xmm_mnew(ntasks * sizeof(WTASK));
    if((taskqueue = iqueue_init()))
    {
        for(i = 0; i < ntasks; i++)
        {
            iqueue_push(taskqueue, i);
            tasks[i].id = i;
            tasks[i].status = 0;
            MUTEX_INIT(tasks[i].mutex);
        }
    }
    return (sbase->add_service(sbase, httpd) | sbase->add_service(sbase, spider));
}

/* stop spider */
static void spider_stop(int sig)
{
    switch (sig) 
    {
        case SIGINT:
        case SIGTERM:
            fprintf(stderr, "spider is interrupted by user.\n");
            running_status = 0;
            if(sbase){sbase->stop(sbase);}
            break;
        default:
            break;
    }
}

/* main */
int main(int argc, char **argv)
{
    char *conf = NULL, ch = 0;
    int is_daemon = 0, i = 0;
    pid_t pid;

    /* get configure file */
    while((ch = getopt(argc, argv, "c:d")) != -1)
    {
        if(ch == 'c') conf = optarg;
        else if(ch == 'd') is_daemon = 1;
    }
    if(conf == NULL)
    {
        fprintf(stderr, "Usage:%s -d -c config_file\n", argv[0]);
        _exit(-1);
    }
    /* locale */
    setlocale(LC_ALL, "C");
    /* signal */
    signal(SIGTERM, &spider_stop);
    signal(SIGINT,  &spider_stop);
    signal(SIGHUP,  &spider_stop);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    if(is_daemon)
    {
        pid = fork();
        switch (pid) {
            case -1:
                perror("fork()");
                exit(EXIT_FAILURE);
                break;
            case 0: /* child process */
                if(setsid() == -1)
                    exit(EXIT_FAILURE);
                break;
            default:/* parent */
                _exit(EXIT_SUCCESS);
                break;
        }
    }
    if((sbase = sbase_init()) == NULL)
    {
        exit(EXIT_FAILURE);
        return -1;
    }
    fprintf(stdout, "Initializing from configure file:%s\n", conf);
    /* Initialize sbase */
    if(sbase_initialize(sbase, conf) != 0 )
    {
        fprintf(stderr, "Initialize from configure file failed\n");
        return -1;
    }
    fprintf(stdout, "Initialized successed\n");
    //fprintf(stdout, "sizeof(XINDEX):%u sizeof(TERMNODE):%u\n", sizeof(XINDEX), sizeof(TERMNODE));
    //while(running_status)sleep(1);
    sbase->running(sbase, 0);
    //sbase->running(sbase, 3600);
    //sbase->running(sbase, 60000000);
    //sbase->stop(sbase);
    sbase->clean(sbase);
    if(http_headers_map) http_headers_map_clean(http_headers_map);
    mime_map_clean(&http_mime_map);
    if(dict)iniparser_free(dict);
    if(tasks)
    {
        for(i = 0; i < ntasks; i++)
        {
            MUTEX_DESTROY(tasks[i].mutex);
        }
        xmm_free(tasks, ntasks * sizeof(WTASK));
    }
    if(taskqueue){iqueue_clean(taskqueue);}
    if(argvmap)mtrie_clean(argvmap);
    if(httpd_index_html_code) free(httpd_index_html_code);
    return 0;
}
