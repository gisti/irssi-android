/*
 chat-completion.c : irssi

    Copyright (C) 1999-2000 Timo Sirainen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "module.h"
#include "signals.h"
#include "commands.h"
#include "misc.h"
#include "levels.h"
#include "settings.h"

#include "chatnets.h"
#include "servers.h"
#include "servers-setup.h"
#include "channels.h"
#include "channels-setup.h"
#include "queries.h"
#include "nicklist.h"

#include "completion.h"
#include "window-items.h"

static int keep_privates_count, keep_publics_count;
static int completion_lowercase;
static const char *completion_char, *cmdchars;
static GSList *global_lastmsgs;
static int completion_auto, completion_strict;

#define SERVER_LAST_MSG_ADD(server, nick) \
	last_msg_add(&((MODULE_SERVER_REC *) MODULE_DATA(server))->lastmsgs, \
		     nick, TRUE, keep_privates_count)

#define CHANNEL_LAST_MSG_ADD(channel, nick, own) \
	last_msg_add(&((MODULE_CHANNEL_REC *) MODULE_DATA(channel))->lastmsgs, \
		     nick, own, keep_publics_count)

static LAST_MSG_REC *last_msg_find(GSList *list, const char *nick)
{
	while (list != NULL) {
		LAST_MSG_REC *rec = list->data;

		if (g_strcasecmp(rec->nick, nick) == 0)
			return rec;
		list = list->next;
	}

	return NULL;
}

static void last_msg_dec_owns(GSList *list)
{
	LAST_MSG_REC *rec;

	while (list != NULL) {
		rec = list->data;
		if (rec->own) rec->own--;

		list = list->next;
	}
}

static void last_msg_add(GSList **list, const char *nick, int own, int max)
{
	LAST_MSG_REC *rec;

	rec = last_msg_find(*list, nick);
	if (rec != NULL) {
		/* msg already exists, update it */
		*list = g_slist_remove(*list, rec);
		if (own)
			rec->own = max;
		else if (rec->own)
                        rec->own--;
	} else {
		rec = g_new(LAST_MSG_REC, 1);
		rec->nick = g_strdup(nick);

		if ((int)g_slist_length(*list) == max) {
			*list = g_slist_remove(*list,
					       g_slist_last(*list)->data);
		}

		rec->own = own ? max : 0;
	}
	rec->time = time(NULL);

        last_msg_dec_owns(*list);

	*list = g_slist_prepend(*list, rec);
}

static void last_msg_destroy(GSList **list, LAST_MSG_REC *rec)
{
	*list = g_slist_remove(*list, rec);

	g_free(rec->nick);
	g_free(rec);
}

void completion_last_message_add(const char *nick)
{
	g_return_if_fail(nick != NULL);

	last_msg_add(&global_lastmsgs, nick, TRUE, keep_privates_count);
}

void completion_last_message_remove(const char *nick)
{
	LAST_MSG_REC *rec;

	g_return_if_fail(nick != NULL);

	rec = last_msg_find(global_lastmsgs, nick);
        if (rec != NULL) last_msg_destroy(&global_lastmsgs, rec);
}

void completion_last_message_rename(const char *oldnick, const char *newnick)
{
	LAST_MSG_REC *rec;

	g_return_if_fail(oldnick != NULL);
	g_return_if_fail(newnick != NULL);

	rec = last_msg_find(global_lastmsgs, oldnick);
	if (rec != NULL) {
		g_free(rec->nick);
                rec->nick = g_strdup(newnick);
	}
}

static void sig_message_public(SERVER_REC *server, const char *msg,
			       const char *nick, const char *address,
			       const char *target)
{
	CHANNEL_REC *channel;
        int own;

	channel = channel_find(server, target);
	if (channel != NULL) {
                own = nick_match_msg(channel, msg, server->nick);
		CHANNEL_LAST_MSG_ADD(channel, nick, own);
	}
}

static void sig_message_join(SERVER_REC *server, const char *channel,
			     const char *nick, const char *address)
{
	CHANNEL_REC *chanrec;

	chanrec = channel_find(server, channel);
	if (chanrec != NULL)
		CHANNEL_LAST_MSG_ADD(chanrec, nick, FALSE);
}

static void sig_message_private(SERVER_REC *server, const char *msg,
				const char *nick, const char *address)
{
	g_return_if_fail(server != NULL);
	g_return_if_fail(nick != NULL);

	SERVER_LAST_MSG_ADD(server, nick);
}

static void sig_message_own_public(SERVER_REC *server, const char *msg,
				   const char *target, const char *origtarget)
{
	CHANNEL_REC *channel;
	NICK_REC *nick;
	char *p, *msgnick;

	g_return_if_fail(server != NULL);
	g_return_if_fail(msg != NULL);
        if (target == NULL) return;

        channel = channel_find(server, target);
	if (channel == NULL)
		return;

	/* channel msg - if first word in line is nick,
	   add it to lastmsgs */
	p = strchr(msg, ' ');
	if (p != NULL && p != msg) {
		msgnick = g_strndup(msg, (int) (p-msg));
		nick = nicklist_find(channel, msgnick);
		if (nick == NULL && msgnick[1] != '\0') {
			/* probably ':' or ',' or some other
			   char after nick, try without it */
			msgnick[strlen(msgnick)-1] = '\0';
			nick = nicklist_find(channel, msgnick);
		}
                g_free(msgnick);
		if (nick != NULL && nick != channel->ownnick)
			CHANNEL_LAST_MSG_ADD(channel, nick->nick, TRUE);
	}
}

static void sig_message_own_private(SERVER_REC *server, const char *msg,
				    const char *target, const char *origtarget)
{
	g_return_if_fail(server != NULL);
	g_return_if_fail(target != NULL);

	if (target != NULL && query_find(server, target) == NULL)
		SERVER_LAST_MSG_ADD(server, target);
}

static void sig_nick_removed(CHANNEL_REC *channel, NICK_REC *nick)
{
        MODULE_CHANNEL_REC *mchannel;
	LAST_MSG_REC *rec;

        mchannel = MODULE_DATA(channel);
	rec = last_msg_find(mchannel->lastmsgs, nick->nick);
	if (rec != NULL) last_msg_destroy(&mchannel->lastmsgs, rec);
}

static void sig_nick_changed(CHANNEL_REC *channel, NICK_REC *nick,
			     const char *oldnick)
{
        MODULE_CHANNEL_REC *mchannel;
	LAST_MSG_REC *rec;

        mchannel = MODULE_DATA(channel);
	rec = last_msg_find(mchannel->lastmsgs, oldnick);
	if (rec != NULL) {
		g_free(rec->nick);
		rec->nick = g_strdup(nick->nick);
	}
}

static int last_msg_cmp(LAST_MSG_REC *m1, LAST_MSG_REC *m2)
{
	return m1->time < m2->time ? 1 : -1;
}

/* Complete /MSG from specified server, or from
   global_lastmsgs if server is NULL */
static void completion_msg_server(GSList **list, SERVER_REC *server,
				  const char *nick, const char *prefix)
{
	LAST_MSG_REC *msg;
	GSList *tmp;
	int len;

	g_return_if_fail(nick != NULL);

	len = strlen(nick);
	tmp = server == NULL ? global_lastmsgs :
		((MODULE_SERVER_REC *) MODULE_DATA(server))->lastmsgs;
	for (; tmp != NULL; tmp = tmp->next) {
		LAST_MSG_REC *rec = tmp->data;

		if (len != 0 && g_strncasecmp(rec->nick, nick, len) != 0)
			continue;

		msg = g_new(LAST_MSG_REC, 1);
		msg->time = rec->time;
		msg->nick = prefix == NULL || *prefix == '\0' ?
			g_strdup(rec->nick) :
			g_strconcat(prefix, " ", rec->nick, NULL);
		*list = g_slist_insert_sorted(*list, msg,
					      (GCompareFunc) last_msg_cmp);
	}
}

/* convert list of LAST_MSG_REC's to list of char* nicks. */
static GList *convert_msglist(GSList *msglist)
{
	GList *list;

	list = NULL;
	while (msglist != NULL) {
		LAST_MSG_REC *rec = msglist->data;

                list = g_list_append(list, rec->nick);
		msglist = g_slist_remove(msglist, rec);
		g_free(rec);
	}

	return list;
}

/* Complete /MSG - if `find_server' is NULL, complete nicks from all servers */
static GList *completion_msg(SERVER_REC *win_server,
			     SERVER_REC *find_server,
			     const char *nick, const char *prefix)
{
	GSList *tmp, *list;
	char *newprefix;

	g_return_val_if_fail(nick != NULL, NULL);
	if (servers == NULL) return NULL;

	list = NULL;
	if (find_server != NULL) {
		completion_msg_server(&list, find_server, nick, prefix);
		return convert_msglist(list);
	}

	completion_msg_server(&list, NULL, nick, prefix);
	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		if (rec == win_server)
			newprefix = g_strdup(prefix);
		else {
			newprefix = prefix == NULL ?
				g_strdup_printf("-%s", rec->tag) :
				g_strdup_printf("%s -%s", prefix, rec->tag);
		}

		completion_msg_server(&list, rec, nick, newprefix);
		g_free_not_null(newprefix);
	}

	return convert_msglist(list);
}

static void complete_from_nicklist(GList **outlist, CHANNEL_REC *channel,
				   const char *nick, const char *suffix)
{
        MODULE_CHANNEL_REC *mchannel;
	GSList *tmp;
        GList *ownlist;
	char *str;
	int len;

	/* go through the last x nicks who have said something in the channel.
	   nicks of all the "own messages" are placed before others */
        ownlist = NULL;
	len = strlen(nick);
        mchannel = MODULE_DATA(channel);
	for (tmp = mchannel->lastmsgs; tmp != NULL; tmp = tmp->next) {
		LAST_MSG_REC *rec = tmp->data;

		if (g_strncasecmp(rec->nick, nick, len) == 0 &&
		    glist_find_icase_string(*outlist, rec->nick) == NULL) {
			str = g_strconcat(rec->nick, suffix, NULL);
			if (completion_lowercase) g_strdown(str);
			if (rec->own)
				ownlist = g_list_append(ownlist, str);
                        else
				*outlist = g_list_append(*outlist, str);
		}
	}

        *outlist = g_list_concat(ownlist, *outlist);
}

static GList *completion_nicks_nonstrict(CHANNEL_REC *channel,
					 const char *nick,
					 const char *suffix)
{
	GSList *nicks, *tmp;
	GList *list;
	char *tnick, *str, *in, *out;
	int len, str_len, tmplen;

	g_return_val_if_fail(channel != NULL, NULL);

	list = NULL;

	/* get all nicks from current channel, strip non alnum chars,
	   compare again and add to completion list on matching */
	len = strlen(nick);
	nicks = nicklist_getnicks(channel);

	str_len = 80; str = g_malloc(str_len+1);
	for (tmp = nicks; tmp != NULL; tmp = tmp->next) {
		NICK_REC *rec = tmp->data;

                tmplen = strlen(rec->nick);
		if (tmplen > str_len) {
                        str_len = tmplen*2;
                        str = g_realloc(str, str_len+1);
		}

		/* remove non alnum chars from nick */
		in = rec->nick; out = str;
		while (*in != '\0') {
			if (i_isalnum(*in))
				*out++ = *in;
                        in++;
		}
                *out = '\0';

		/* add to list if 'cleaned' nick matches */
		if (g_strncasecmp(str, nick, len) == 0) {
			tnick = g_strconcat(rec->nick, suffix, NULL);
			if (completion_lowercase)
				g_strdown(tnick);

			if (glist_find_icase_string(list, tnick) == NULL)
				list = g_list_append(list, tnick);
			else
                                g_free(tnick);
		}

	}
        g_free(str);
	g_slist_free(nicks);

	return list;
}

static GList *completion_channel_nicks(CHANNEL_REC *channel, const char *nick,
				       const char *suffix)
{
	GSList *nicks, *tmp;
	GList *list;
	char *str;
	int len;

	g_return_val_if_fail(channel != NULL, NULL);
	g_return_val_if_fail(nick != NULL, NULL);
	if (*nick == '\0') return NULL;

	if (suffix != NULL && *suffix == '\0')
		suffix = NULL;

	/* put first the nicks who have recently said something */
	list = NULL;
	complete_from_nicklist(&list, channel, nick, suffix);

	/* and add the rest of the nicks too */
	len = strlen(nick);
	nicks = nicklist_getnicks(channel);
	for (tmp = nicks; tmp != NULL; tmp = tmp->next) {
		NICK_REC *rec = tmp->data;

		if (g_strncasecmp(rec->nick, nick, len) == 0 &&
		    rec != channel->ownnick) {
			str = g_strconcat(rec->nick, suffix, NULL);
			if (completion_lowercase)
				g_strdown(str);
                        if (glist_find_icase_string(list, str) == NULL)
				list = g_list_append(list, str);
			else
                                g_free(str);
		}
	}
	g_slist_free(nicks);

	/* remove non alphanum chars from nick and search again in case
	   list is still NULL ("foo<tab>" would match "_foo_" f.e.) */
	if (!completion_strict)
		list = g_list_concat(list, completion_nicks_nonstrict(channel, nick, suffix));
	return list;
}

/* append all strings in list2 to list1 that already aren't there and
   free list2 */
static GList *completion_joinlist(GList *list1, GList *list2)
{
	GList *old;

	old = list2;
	while (list2 != NULL) {
		if (!glist_find_icase_string(list1, list2->data))
			list1 = g_list_append(list1, list2->data);
		else
			g_free(list2->data);

		list2 = list2->next;
	}

	g_list_free(old);
	return list1;
}

GList *completion_get_channels(SERVER_REC *server, const char *word)
{
	GList *list;
	GSList *tmp;
	int len;

	g_return_val_if_fail(word != NULL, NULL);
	g_return_val_if_fail(*word != '\0', NULL);

	len = strlen(word);
	list = NULL;

	/* first get the joined channels */
	tmp = server == NULL ? NULL : server->channels;
	for (; tmp != NULL; tmp = tmp->next) {
		CHANNEL_REC *rec = tmp->data;

		if (g_strncasecmp(rec->name, word, len) == 0)
			list = g_list_append(list, g_strdup(rec->name));
	}

	/* get channels from setup */
	for (tmp = setupchannels; tmp != NULL; tmp = tmp->next) {
		CHANNEL_SETUP_REC *rec = tmp->data;

		if (g_strncasecmp(rec->name, word, len) == 0 &&
		    glist_find_icase_string(list, rec->name) == NULL)
			list = g_list_append(list, g_strdup(rec->name));

	}

	return list;
}

static void complete_window_nicks(GList **list, WINDOW_REC *window,
                                  const char *word, const char *linestart)
{
        CHANNEL_REC *channel;
        GList *tmplist;
        GSList *tmp;
        const char *nicksuffix;

        nicksuffix = *linestart != '\0' ? NULL : completion_char;

        channel = CHANNEL(window->active);

        /* first the active channel */
        if (channel != NULL) {
                tmplist = completion_channel_nicks(channel, word, nicksuffix);
                *list = completion_joinlist(*list, tmplist);
        }

        if (nicksuffix != NULL) {
                /* completing nick at the start of line - probably answering
                   to some other nick, don't even try to complete from
                   non-active channels */
                return;
        }

        /* then the rest */
        for (tmp = window->items; tmp != NULL; tmp = tmp->next) {
                channel = CHANNEL(tmp->data);
                if (channel != NULL && tmp->data != window->active) {
                        tmplist = completion_channel_nicks(channel, word,
                                                           nicksuffix);
                        *list = completion_joinlist(*list, tmplist);
                }
        }
}

static void sig_complete_word(GList **list, WINDOW_REC *window,
			      const char *word, const char *linestart,
			      int *want_space)
{
	SERVER_REC *server;
	CHANNEL_REC *channel;
	QUERY_REC *query;
	char *prefix;

	g_return_if_fail(list != NULL);
	g_return_if_fail(window != NULL);
	g_return_if_fail(word != NULL);
	g_return_if_fail(linestart != NULL);

	server = window->active_server;
	if (server == NULL && servers != NULL)
		server = servers->data;

	if (server != NULL && server_ischannel(server, word)) {
		/* probably completing a channel name */
		*list = completion_get_channels(window->active_server, word);
                return;
	}

	server = window->active_server;
	if (server == NULL || !server->connected)
		return;

	if (*linestart == '\0' && *word == '\0') {
		/* pressed TAB at the start of line - add /MSG */
                prefix = g_strdup_printf("%cmsg", *cmdchars);
		*list = completion_msg(server, NULL, "", prefix);
		if (*list == NULL)
			*list = g_list_append(*list, g_strdup(prefix));
		g_free(prefix);

		signal_stop();
		return;
	}

	channel = CHANNEL(window->active);
	query = QUERY(window->active);
	if (channel == NULL && query != NULL &&
	    g_strncasecmp(word, query->name, strlen(word)) == 0) {
		/* completion in query */
                *list = g_list_append(*list, g_strdup(query->name));
	} else if (channel != NULL) {
		/* nick completion .. we could also be completing a nick
		   after /MSG from nicks in channel */
		complete_window_nicks(list, window, word, linestart);
	} else if (window->level & MSGLEVEL_MSGS) {
		/* msgs window, complete /MSG nicks */
                *list = g_list_concat(completion_msg(server, NULL, word, NULL), *list);
	}

	if (*list != NULL) signal_stop();
}

static SERVER_REC *line_get_server(const char *line)
{
	SERVER_REC *server;
	char *tag, *ptr;

	g_return_val_if_fail(line != NULL, NULL);
	if (*line != '-') return NULL;

	/* -option found - should be server tag */
	tag = g_strdup(line+1);
	ptr = strchr(tag, ' ');
	if (ptr != NULL) *ptr = '\0';

	server = server_find_tag(tag);

	g_free(tag);
	return server;
}

static void sig_complete_msg(GList **list, WINDOW_REC *window,
			     const char *word, const char *line,
			     int *want_space)
{
	SERVER_REC *server, *msgserver;

	g_return_if_fail(list != NULL);
	g_return_if_fail(word != NULL);
	g_return_if_fail(line != NULL);

	server = window->active_server;
	if (server == NULL || !server->connected)
		return;

	msgserver = line_get_server(line);
	*list = completion_msg(server, msgserver, word, NULL);
	if (*list != NULL) signal_stop();
}

static void sig_erase_complete_msg(WINDOW_REC *window, const char *word,
				   const char *line)
{
	SERVER_REC *server;
	MODULE_SERVER_REC *mserver;
        GSList *tmp;

	server = line_get_server(line);
	if (server == NULL){
		server = window->active_server;
		if (server == NULL)
                        return;
	}

	if (*word == '\0')
		return;

        /* check from global list */
	completion_last_message_remove(word);

	/* check from server specific list */
	if (server != NULL) {
		mserver = MODULE_DATA(server);
		for (tmp = mserver->lastmsgs; tmp != NULL; tmp = tmp->next) {
			LAST_MSG_REC *rec = tmp->data;

			if (g_strcasecmp(rec->nick, word) == 0) {
				last_msg_destroy(&mserver->lastmsgs, rec);
                                break;
			}
		}

	}
}

GList *completion_get_chatnets(const char *word)
{
	GList *list;
	GSList *tmp;
	int len;

	g_return_val_if_fail(word != NULL, NULL);

	len = strlen(word);
	list = NULL;

	for (tmp = chatnets; tmp != NULL; tmp = tmp->next) {
		CHATNET_REC *rec = tmp->data;

		if (g_strncasecmp(rec->name, word, len) == 0)
			list = g_list_append(list, g_strdup(rec->name));
	}

	return list;
}

GList *completion_get_servers(const char *word)
{
	GList *list;
	GSList *tmp;
	int len;

	g_return_val_if_fail(word != NULL, NULL);

	len = strlen(word);
	list = NULL;

	for (tmp = setupservers; tmp != NULL; tmp = tmp->next) {
		SERVER_SETUP_REC *rec = tmp->data;

		if (g_strncasecmp(rec->address, word, len) == 0) 
			list = g_list_append(list, g_strdup(rec->address));
	}

	return list;
}

static void sig_complete_connect(GList **list, WINDOW_REC *window,
				 const char *word, const char *line, 
				 int *want_space)
{
	g_return_if_fail(list != NULL);
	g_return_if_fail(word != NULL);

	*list = completion_get_chatnets(word);
	*list = g_list_concat(*list, completion_get_servers(word));
	if (*list != NULL) signal_stop();
}

static void sig_complete_topic(GList **list, WINDOW_REC *window,
			       const char *word, const char *line,
			       int *want_space)
{
	const char *topic;

	g_return_if_fail(list != NULL);
	g_return_if_fail(word != NULL);

	if (*word == '\0' && IS_CHANNEL(window->active)) {
		topic = CHANNEL(window->active)->topic;
		if (topic != NULL) {
			*list = g_list_append(NULL, g_strdup(topic));
                        signal_stop();
		}
	}
}

/* expand \n, \t and \\ */
static char *expand_escapes(const char *line, SERVER_REC *server,
			    WI_ITEM_REC *item)
{
	char *ptr, *ret;
        int chr;

	ret = ptr = g_malloc(strlen(line)+1);
	for (; *line != '\0'; line++) {
		if (*line != '\\') {
			*ptr++ = *line;
			continue;
		}

		line++;
		if (*line == '\0') {
			*ptr++ = '\\';
			break;
		}

		chr = expand_escape(&line);
		if (chr == '\r' || chr == '\n') {
			/* newline .. we need to send another "send text"
			   event to handle it (or actually the text before
			   the newline..) */
			if (ret != ptr) {
				*ptr = '\0';
				signal_emit("send text", 3, ret, server, item);
				ptr = ret;
			}
		} else if (chr != -1) {
                        /* escaping went ok */
			*ptr++ = chr;
		} else {
                        /* unknown escape, add it as-is */
			*ptr++ = '\\';
			*ptr++ = *line;
		}
	}

	*ptr = '\0';
	return ret;
}

static char *auto_complete(CHANNEL_REC *channel, const char *line)
{
	GList *comp;
	const char *p;
        char *nick, *ret;

	p = strstr(line, completion_char);
	if (p == NULL)
		return NULL;

        nick = g_strndup(line, (int) (p-line));

        ret = NULL;
	if (nicklist_find(channel, nick) == NULL) {
                /* not an exact match, use the first possible completion */
		comp = completion_channel_nicks(channel, nick, NULL);
		if (comp != NULL) {
			ret = g_strconcat(comp->data, p, NULL);
			g_list_foreach(comp, (GFunc) g_free, NULL);
			g_list_free(comp);
		}
	}

	g_free(nick);

        return ret;
}

static void event_text(const char *data, SERVER_REC *server, WI_ITEM_REC *item)
{
	char *line, *str;

	g_return_if_fail(data != NULL);
	if (item == NULL) return;

	line = settings_get_bool("expand_escapes") ?
		expand_escapes(data, server, item) : g_strdup(data);

	/* check for automatic nick completion */
	if (completion_auto && IS_CHANNEL(item)) {
		str = auto_complete(CHANNEL(item), line);
		if (str != NULL) {
			g_free(line);
                        line = str;
		}
	}

	str = g_strdup_printf(IS_CHANNEL(item) ? "-channel %s %s" :
			      IS_QUERY(item) ? "-nick %s %s" : "%s %s",
			      item->name, line);

	signal_emit("command msg", 3, str, server, item);

	g_free(str);
	g_free(line);

	signal_stop();
}

static void sig_server_disconnected(SERVER_REC *server)
{
	MODULE_SERVER_REC *mserver;

	g_return_if_fail(server != NULL);

        mserver = MODULE_DATA(server);
	while (mserver->lastmsgs)
                last_msg_destroy(&mserver->lastmsgs, mserver->lastmsgs->data);
}

static void sig_channel_destroyed(CHANNEL_REC *channel)
{
	MODULE_CHANNEL_REC *mchannel;

	g_return_if_fail(channel != NULL);

        mchannel = MODULE_DATA(channel);
	while (mchannel->lastmsgs != NULL) {
		last_msg_destroy(&mchannel->lastmsgs,
				 mchannel->lastmsgs->data);
	}
}

static void read_settings(void)
{
	keep_privates_count = settings_get_int("completion_keep_privates");
	keep_publics_count = settings_get_int("completion_keep_publics");
	completion_lowercase = settings_get_bool("completion_nicks_lowercase");
	completion_char = settings_get_str("completion_char");
	cmdchars = settings_get_str("cmdchars");
	completion_auto = settings_get_bool("completion_auto");
	completion_strict = settings_get_bool("completion_strict");

	if (*completion_char == '\0') {
                /* this would break.. */
		completion_auto = FALSE;
	}
}

void chat_completion_init(void)
{
	settings_add_str("completion", "completion_char", ":");
	settings_add_bool("completion", "completion_auto", FALSE);
	settings_add_int("completion", "completion_keep_publics", 50);
	settings_add_int("completion", "completion_keep_privates", 10);
	settings_add_bool("completion", "expand_escapes", FALSE);
	settings_add_bool("completion", "completion_nicks_lowercase", FALSE);
	settings_add_bool("completion", "completion_strict", FALSE);

	read_settings();
	signal_add("complete word", (SIGNAL_FUNC) sig_complete_word);
	signal_add("complete command msg", (SIGNAL_FUNC) sig_complete_msg);
	signal_add("complete command query", (SIGNAL_FUNC) sig_complete_msg);
	signal_add("complete erase command msg", (SIGNAL_FUNC) sig_erase_complete_msg);
	signal_add("complete erase command query", (SIGNAL_FUNC) sig_erase_complete_msg);
	signal_add("complete command connect", (SIGNAL_FUNC) sig_complete_connect);
	signal_add("complete command server", (SIGNAL_FUNC) sig_complete_connect);
	signal_add("complete command topic", (SIGNAL_FUNC) sig_complete_topic);
	signal_add("message public", (SIGNAL_FUNC) sig_message_public);
	signal_add("message join", (SIGNAL_FUNC) sig_message_join);
	signal_add("message private", (SIGNAL_FUNC) sig_message_private);
	signal_add("message own_public", (SIGNAL_FUNC) sig_message_own_public);
	signal_add("message own_private", (SIGNAL_FUNC) sig_message_own_private);
	signal_add("nicklist remove", (SIGNAL_FUNC) sig_nick_removed);
	signal_add("nicklist changed", (SIGNAL_FUNC) sig_nick_changed);
	signal_add("send text", (SIGNAL_FUNC) event_text);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_add("channel destroyed", (SIGNAL_FUNC) sig_channel_destroyed);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
}

void chat_completion_deinit(void)
{
	while (global_lastmsgs != NULL)
		last_msg_destroy(&global_lastmsgs, global_lastmsgs->data);

	signal_remove("complete word", (SIGNAL_FUNC) sig_complete_word);
	signal_remove("complete command msg", (SIGNAL_FUNC) sig_complete_msg);
	signal_remove("complete command query", (SIGNAL_FUNC) sig_complete_msg);
	signal_remove("complete erase command msg", (SIGNAL_FUNC) sig_erase_complete_msg);
	signal_remove("complete erase command query", (SIGNAL_FUNC) sig_erase_complete_msg);
	signal_remove("complete command connect", (SIGNAL_FUNC) sig_complete_connect);
	signal_remove("complete command server", (SIGNAL_FUNC) sig_complete_connect);
	signal_remove("complete command topic", (SIGNAL_FUNC) sig_complete_topic);
	signal_remove("message public", (SIGNAL_FUNC) sig_message_public);
	signal_remove("message join", (SIGNAL_FUNC) sig_message_join);
	signal_remove("message private", (SIGNAL_FUNC) sig_message_private);
	signal_remove("message own_public", (SIGNAL_FUNC) sig_message_own_public);
	signal_remove("message own_private", (SIGNAL_FUNC) sig_message_own_private);
	signal_remove("nicklist remove", (SIGNAL_FUNC) sig_nick_removed);
	signal_remove("nicklist changed", (SIGNAL_FUNC) sig_nick_changed);
	signal_remove("send text", (SIGNAL_FUNC) event_text);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_remove("channel destroyed", (SIGNAL_FUNC) sig_channel_destroyed);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
}
