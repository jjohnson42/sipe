/**
 * @file sipe-domino.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2010 pier11 <pier11@operamail.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
For communication with Lotus Domino groupware server.

Server requirements: Domino 5.0.2 and above with Web Access.

0) May be try to read user's notes.ini for mail database name.

1) Authenticates to server (HTTPS POST, plaintext login/password over SSL)
https://[domino_server]/[databasename].nsf/?Login
Content-Type=application/x-www-form-urlencoded
Username=[email]&Password=[password] (params are url-encoded)
Saves auth cookie.
Set-Cookie=DomAuthSessId=17D0428F7B9D57D4D0B064AE42FD21F9; path=/

2) Queries Calendar data (HTTPS GET, result is XML)
https://[domino_server]/[databasename].nsf/[viewname]?ReadViewEntries
https://[domino_server]/[databasename].nsf/($Calendar)?ReadViewEntries&KeyType=time&StartKey=20090805T000000Z&UntilKey=20090806T000000Z&Count=-1&TZType=UTC
Uses auth cookie.
Cookie=DomAuthSessId=17D0428F7B9D57D4D0B064AE42FD21F9

It will be able to retrieve our Calendar information (Meetings schedule,
subject and location) from Lotus Domino for subsequent publishing.

Ref. for more implementation details:
https://sourceforge.net/tracker/?func=detail&aid=2945346&group_id=194563&atid=949934

Similar functionality for iCalendar/CalDAV/Google would be great to implement too.
*/

#include <glib.h>

/* @TODO replace purple_url_encode() with non-purple equiv. */
#include "util.h"

#include "sipe-common.h"
#include "sipe-core.h"
#include "sipe.h"
#include "sipe-backend.h"
#include "sipe-utils.h"
#include "sipe-cal.h"
#include "sipe-domino.h"
#include "http-conn.h"

/**
 * POST request for Login to Domino server
 * @param email      (%s) Should be URL-encoded. Ex.: alice@cosmo.local
 * @param password   (%s) Should be URL-encoded.
 */
#define SIPE_DOMINO_LOGIN_REQUEST \
"Username=%s&Password=%s"

/**
 * GET request to Domino server
 * to obtain our Calendar information.
 * @param start_time (%s) Ex.: 20090805T000000Z
 * @param end_time   (%s) Ex.: 20090806T000000Z
 */
#define SIPE_DOMINO_CALENDAR_REQUEST \
"/($Calendar)?ReadViewEntries&KeyType=time&StartKey=%s&UntilKey=%s&Count=-1&TZType=UTC"


static void
sipe_domino_process_calendar_response(int return_code,
				 const char *body,
				 HttpConn *conn,
				 void *data)
{
	struct sipe_calendar *cal = data;

	SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_process_calendar_response: cb started.");

	http_conn_set_close(conn);
	cal->http_conn = NULL;

	if (return_code == 200 && body) {
		SIPE_DEBUG_INFO("sipe_domino_process_calendar_response: SUCCESS, ret=%d", return_code);

	} else if (return_code < 0) {
		SIPE_DEBUG_INFO("sipe_domino_process_calendar_response: rather FAILURE, ret=%d", return_code);
	}
}

/* Domino doesn't like '-' and ':' in ISO timestamps */
static gchar *
sipe_domino_time_to_str(time_t timestamp)
{
	char *res, *tmp;

	res = sipe_utils_time_to_str(timestamp);
	res = sipe_utils_str_replace((tmp = res), "-", "");
	g_free(tmp);
	res = sipe_utils_str_replace((tmp = res), ":", "");
	g_free(tmp);

	return res;
}

static void
sipe_domino_do_calendar_request(struct sipe_calendar *cal)
{
	if (cal->as_url) {
		char *url_req;
		char *url;
		time_t end;
		time_t now = time(NULL);
		char *start_str;
		char *end_str;
		struct tm *now_tm;

		SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_do_calendar_request: going Calendar req.");

		now_tm = gmtime(&now);
		/* start -1 day, 00:00:00 */
		now_tm->tm_sec = 0;
		now_tm->tm_min = 0;
		now_tm->tm_hour = 0;
		cal->fb_start = sipe_mktime_tz(now_tm, "UTC");
		cal->fb_start -= 24*60*60;
		/* end = start + 4 days - 1 sec */
		end = cal->fb_start + 4*(24*60*60) - 1;

		start_str = sipe_domino_time_to_str(cal->fb_start);
		end_str = sipe_domino_time_to_str(end);

		url_req = g_strdup_printf(SIPE_DOMINO_CALENDAR_REQUEST, start_str, end_str);
		g_free(start_str);
		g_free(end_str);

		url = g_strconcat(cal->as_url, url_req, NULL);
		g_free(url_req);
		if (!cal->http_conn || http_conn_is_closed(cal->http_conn)) {
			cal->http_conn = http_conn_create(
					 cal->account,
					 HTTP_CONN_GET,
					 HTTP_CONN_SSL,
					 HTTP_CONN_NO_REDIRECT,
					 url,
					 NULL, /* body */
					 NULL, /* content-type */
					 cal->auth,
					 sipe_domino_process_calendar_response,
					 cal);
		} else {
			http_conn_send(cal->http_conn,
				       HTTP_CONN_GET,
				       url,
				       NULL, /* body */
				       NULL, /* content-type */
				       sipe_domino_process_calendar_response,
				       cal);
		}
		g_free(url);
	}
}

static void
sipe_domino_process_login_response(int return_code,
				   /* temporary? */
				   SIPE_UNUSED_PARAMETER const char *body,
				   SIPE_UNUSED_PARAMETER HttpConn *conn,
				   void *data)
{
	struct sipe_calendar *cal = data;

	SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_process_login_response: cb started.");

	if (return_code >= 200 && return_code < 400) {
		SIPE_DEBUG_INFO("sipe_domino_process_login_response: rather SUCCESS, ret=%d", return_code);
		
		/* next query */
		sipe_domino_do_calendar_request(cal);
		
	} else if (return_code < 0 || return_code >= 400) {
		SIPE_DEBUG_INFO("sipe_domino_process_login_response: rather FAILURE, ret=%d", return_code);
		//cal->http_conn = NULL;
		
		/* stop here */
		//cal->is_disabled = TRUE;

		//http_conn_set_close(conn);
		//cal->http_conn = NULL;
	}
}

static void
sipe_domino_do_login_request(struct sipe_calendar *cal)
{
	if (cal->as_url) {
		char *body;
		const char *content_type = "application/x-www-form-urlencoded";
		char *login_url = g_strconcat(cal->as_url, "/?Login", NULL);
		char *user;
		char *password;

		SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_do_login_request: going Login req.");
		
		if (!cal->auth) return;
		
		/* @TODO replace purple_url_encode() with non-purple equiv. */
		user     = g_strdup(purple_url_encode(cal->auth->user));
		password = g_strdup(purple_url_encode(cal->auth->password));

		body = g_strdup_printf(SIPE_DOMINO_LOGIN_REQUEST, user, password);
		g_free(user);
		g_free(password);
		
		cal->http_conn = http_conn_create(cal->account,
						  HTTP_CONN_POST,
						  HTTP_CONN_SSL,
						  HTTP_CONN_NO_REDIRECT,
						  login_url,
						  body,
						  content_type,
						  cal->auth,
						  sipe_domino_process_login_response,
						  cal);
		g_free(login_url);
		g_free(body);
	}
}

void
sipe_domino_update_calendar(struct sipe_account_data *sip)
{

	SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_update_calendar: started.");

	sipe_cal_calendar_init(sip, NULL);

	if (sip->cal->is_disabled) {
		SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_update_calendar: disabled, exiting.");
		return;
	}

	sipe_domino_do_login_request(sip->cal);

	SIPE_DEBUG_INFO_NOFORMAT("sipe_domino_update_calendar: finished.");
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
