#define MODULE_LOG_PREFIX "sched"

#include "globals.h"

#ifdef WEBIF

#include "module-sched.h"
#include "oscam-net.h"
#include "oscam-string.h"
#include "oscam-time.h"
#include "cscrypt/md5.h"

extern int32_t ssl_active;

#define SCHED_HTTP_QUERY_MAX 1024
#define SCHED_HTTP_RESP_MAX  4096

static const char *extract_quoted(const char *hdr, const char *param, char *out, size_t len)
{
	char search[48];
	const char *start, *end;
	size_t n;
	snprintf(search, sizeof(search), "%s=\"", param);
	start = strstr(hdr, search);
	if(!start) { return NULL; }
	start += strlen(search);
	end = strchr(start, '"');
	if(!end) { return NULL; }
	n = (size_t)(end - start);
	if(n >= len) { n = len - 1; }
	memcpy(out, start, n);
	out[n] = '\0';
	return out;
}

static void md5_hex(const char *in, char *hex)
{
	unsigned char h[MD5_DIGEST_LENGTH];
	int32_t i;
	MD5((const unsigned char *)in, strlen(in), h);
	for(i = 0; i < MD5_DIGEST_LENGTH; i++) { snprintf(hex + i * 2, 3, "%02x", h[i]); }
}

static void digest_header(const char *user, const char *pass, const char *uri, const char *realm,
						  const char *nonce, const char *qop, const char *opaque, char *out, size_t outlen)
{
	char tmp[1400], ha1[33], ha2[33], resp[33], cnonce[40];
	snprintf(tmp, sizeof(tmp), "%s:%s:%s", user, realm, pass);
	md5_hex(tmp, ha1);
	snprintf(tmp, sizeof(tmp), "GET:%s", uri);
	md5_hex(tmp, ha2);
	snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)cs_time(), (unsigned long)(uintptr_t)pthread_self());

	if(qop[0])
		{ snprintf(tmp, sizeof(tmp), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2); }
	else
		{ snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, nonce, ha2); }
	md5_hex(tmp, resp);

	if(qop[0])
	{
		snprintf(out, outlen, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\", response=\"%s\"%s%s%s",
				 user, realm, nonce, uri, cnonce, resp, opaque[0] ? ", opaque=\"" : "", opaque, opaque[0] ? "\"" : "");
	}
	else
	{
		snprintf(out, outlen, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"", user, realm, nonce, uri, resp);
	}
}

static int32_t http_get(int32_t port, const char *uri, const char *auth, char *resp, size_t respsz)
{
	struct SOCKADDR sad;
	struct timeval tv;
	char req[SCHED_HTTP_QUERY_MAX + 1500];
	int32_t sock, len;
	size_t sent = 0, total = 0;

	memset(&sad, 0, sizeof(sad));
	SIN_GET_FAMILY(sad) = DEFAULT_AF;
	if(IP_ISSET(cfg.http_srvip)) { IP_ASSIGN(SIN_GET_ADDR(sad), cfg.http_srvip); }
	else if(IP_ISSET(cfg.srvip)) { IP_ASSIGN(SIN_GET_ADDR(sad), cfg.srvip); }
	else { set_localhost_ip(&SIN_GET_ADDR(sad)); }
	SIN_GET_PORT(sad) = htons(port);

	sock = socket(DEFAULT_AF, SOCK_STREAM, IPPROTO_TCP);
	if(sock < 0) { return -1; }
	tv.tv_sec = 10; tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if(connect(sock, (struct sockaddr *)&sad, sizeof(sad)) < 0) { close(sock); return -1; }

	len = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n%s%s%sConnection: close\r\n\r\n",
				   uri, port, auth ? "Authorization: " : "", auth ? auth : "", auth ? "\r\n" : "");
	if(len < 0 || (size_t)len >= sizeof(req)) { close(sock); return -1; }
	while(sent < (size_t)len)
	{
		ssize_t n = send(sock, req + sent, (size_t)len - sent, 0);
		if(n <= 0) { close(sock); return -1; }
		sent += (size_t)n;
	}
	while(total < respsz - 1)
	{
		ssize_t n = recv(sock, resp + total, respsz - 1 - total, 0);
		if(n <= 0) { break; }
		total += (size_t)n;
	}
	close(sock);
	if(!total) { return -1; }
	resp[total] = '\0';
	{
		const char *h = strstr(resp, "HTTP/");
		const char *sp = h ? strchr(h, ' ') : NULL;
		return sp ? atoi(sp + 1) : 0;
	}
}

int32_t sched_api_call(const char *query, char *msg, size_t msglen)
{
	char uri[SCHED_HTTP_QUERY_MAX + 2], resp[SCHED_HTTP_RESP_MAX];
	int32_t port = cfg.http_port, code;

	if(!query || !*query) { snprintf(msg, msglen, "empty API query"); return -1; }
	if(strpbrk(query, "\r\n ")) { snprintf(msg, msglen, "API query must not contain spaces or line breaks"); return -1; }
	if(strlen(query) > SCHED_HTTP_QUERY_MAX) { snprintf(msg, msglen, "API query too long"); return -1; }
	if(port <= 0) { snprintf(msg, msglen, "WebIF is not enabled (httpport)"); return -1; }
	if(ssl_active) { snprintf(msg, msglen, "API calls need plain HTTP (WebIF uses SSL)"); return -1; }

	snprintf(uri, sizeof(uri), "%s%s", *query == '/' ? "" : "/", query);
	code = http_get(port, uri, NULL, resp, sizeof(resp));
	if(code < 0) { snprintf(msg, msglen, "cannot connect to WebIF on port %d (check httpallowed)", port); return -1; }

	if(code == 401 && cfg.http_user && *cfg.http_user)
	{
		char realm[160] = "", nonce[160] = "", qop[32] = "", opaque[160] = "", auth[2800];
		if(extract_quoted(resp, "realm", realm, sizeof(realm)) && extract_quoted(resp, "nonce", nonce, sizeof(nonce)))
		{
			extract_quoted(resp, "qop", qop, sizeof(qop));
			extract_quoted(resp, "opaque", opaque, sizeof(opaque));
			digest_header(cfg.http_user, cfg.http_pwd ? cfg.http_pwd : "", uri, realm, nonce, qop, opaque, auth, sizeof(auth));
			code = http_get(port, uri, auth, resp, sizeof(resp));
			if(code < 0) { snprintf(msg, msglen, "authenticated request failed"); return -1; }
		}
	}
	if(code >= 200 && code < 300) { return 0; }
	if(code == 401) { snprintf(msg, msglen, "WebIF rejected the login (httpuser/httppwd)"); return -1; }
	if(code == 403) { snprintf(msg, msglen, "WebIF refused this machine: add 127.0.0.1 (or the server IP) to httpallowed"); return -1; }
	snprintf(msg, msglen, "WebIF answered HTTP %d", code);
	return -1;
}

#endif
