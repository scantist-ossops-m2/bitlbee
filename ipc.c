  /********************************************************************\
  * BitlBee -- An IRC to other IM-networks gateway                     *
  *                                                                    *
  * Copyright 2002-2004 Wilmer van der Gaast and others                *
  \********************************************************************/

/* IPC - communication between BitlBee processes                        */

/*
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License with
  the Debian GNU/Linux distribution in /usr/share/common-licenses/GPL;
  if not, write to the Free Software Foundation, Inc., 59 Temple Place,
  Suite 330, Boston, MA  02111-1307  USA
*/

#define BITLBEE_CORE
#include "bitlbee.h"
#include "ipc.h"
#include "commands.h"

GSList *child_list = NULL;


static int ipc_master_cmd_die( irc_t *irc, char **cmd )
{
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
		ipc_to_children_str( "DIE\r\n" );
	
	bitlbee_shutdown( NULL );
	
	return 1;
}

static int ipc_master_cmd_rehash( irc_t *irc, char **cmd )
{
	runmode_t oldmode;
	
	oldmode = global.conf->runmode;
	
	g_free( global.conf );
	global.conf = conf_load( 0, NULL );
	
	if( global.conf->runmode != oldmode )
	{
		log_message( LOGLVL_WARNING, "Can't change RunMode setting at runtime, restoring original setting" );
		global.conf->runmode = oldmode;
	}
	
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
		ipc_to_children( cmd );
	
	return 1;
}

static const command_t ipc_master_commands[] = {
	{ "die",        0, ipc_master_cmd_die,        0 },
	{ "wallops",    1, NULL,                      IPC_CMD_TO_CHILDREN },
	{ "lilo",       1, NULL,                      IPC_CMD_TO_CHILDREN },
	{ "rehash",     0, ipc_master_cmd_rehash,     0 },
	{ "kill",       2, NULL,                      IPC_CMD_TO_CHILDREN },
	{ NULL }
};


static int ipc_child_cmd_die( irc_t *irc, char **cmd )
{
	bitlbee_shutdown( NULL );
	
	return 1;
}

static int ipc_child_cmd_wallops( irc_t *irc, char **cmd )
{
	if( irc->status < USTATUS_LOGGED_IN )
		return 1;
	
	if( strchr( irc->umode, 'w' ) )
		irc_write( irc, ":%s WALLOPS :%s", irc->myhost, cmd[1] );
	
	return 1;
}

static int ipc_child_cmd_lilo( irc_t *irc, char **cmd )
{
	if( irc->status < USTATUS_LOGGED_IN )
		return 1;
	
	if( strchr( irc->umode, 's' ) )
		irc_write( irc, ":%s NOTICE %s :%s", irc->myhost, irc->nick, cmd[1] );
	
	return 1;
}

static int ipc_child_cmd_rehash( irc_t *irc, char **cmd )
{
	runmode_t oldmode;
	
	oldmode = global.conf->runmode;
	
	g_free( global.conf );
	global.conf = conf_load( 0, NULL );
	
	global.conf->runmode = oldmode;
	
	return 1;
}

static int ipc_child_cmd_kill( irc_t *irc, char **cmd )
{
	if( irc->status < USTATUS_LOGGED_IN )
		return 1;
	
	if( nick_cmp( cmd[1], irc->nick ) != 0 )
		return 1;	/* It's not for us. */
	
	irc_write( irc, ":%s!%s@%s KILL %s :%s", irc->mynick, irc->mynick, irc->myhost, irc->nick, cmd[2] );
	g_io_channel_close( irc->io_channel );
	
	return 0;
}

static const command_t ipc_child_commands[] = {
	{ "die",        0, ipc_child_cmd_die,         0 },
	{ "wallops",    1, ipc_child_cmd_wallops,     0 },
	{ "lilo",       1, ipc_child_cmd_lilo,        0 },
	{ "rehash",     0, ipc_child_cmd_rehash,      0 },
	{ "kill",       2, ipc_child_cmd_kill,        0 },
	{ NULL }
};


static void ipc_command_exec( void *data, char **cmd, const command_t *commands )
{
	int i;
	
	if( !cmd[0] )
		return;
	
	for( i = 0; commands[i].command; i ++ )
		if( g_strcasecmp( commands[i].command, cmd[0] ) == 0 )
		{
			if( commands[i].flags & IPC_CMD_TO_CHILDREN )
				ipc_to_children( cmd );
			else
				commands[i].execute( data, cmd );
			
			return;
		}
}

static char *ipc_readline( int fd )
{
	char *buf, *eol;
	int size;
	
	buf = g_new0( char, 513 );
	
	/* Because this is internal communication, it should be pretty safe
	   to just peek at the message, find its length (by searching for the
	   end-of-line) and then just read that message. With internal
	   sockets and limites message length, messages should always be
	   complete. Saves us quite a lot of code and buffering. */
	size = recv( fd, buf, 512, MSG_PEEK );
	if( size == 0 || ( size < 0 && !sockerr_again() ) )
		return NULL;
	else if( size < 0 ) /* && sockerr_again() */
		return( g_strdup( "" ) );
	else
		buf[size] = 0;
	
	eol = strstr( buf, "\r\n" );
	if( eol == NULL )
		return NULL;
	else
		size = eol - buf + 2;
	
	g_free( buf );
	buf = g_new0( char, size + 1 );
	
	if( recv( fd, buf, size, 0 ) != size )
		return NULL;
	else
		buf[size-2] = 0;
	
	return buf;
}

void ipc_master_read( gpointer data, gint source, GaimInputCondition cond )
{
	char *buf, **cmd;
	
	if( ( buf = ipc_readline( source ) ) )
	{
		cmd = irc_parse_line( buf );
		if( cmd )
			ipc_command_exec( data, cmd, ipc_master_commands );
	}
	else
	{
		GSList *l;
		struct bitlbee_child *c;
		
		for( l = child_list; l; l = l->next )
		{
			c = l->data;
			if( c->ipc_fd == source )
			{
				close( c->ipc_fd );
				gaim_input_remove( c->ipc_inpa );
				g_free( c );
				
				child_list = g_slist_remove( child_list, l );
				
				break;
			}
		}
	}
}

void ipc_child_read( gpointer data, gint source, GaimInputCondition cond )
{
	char *buf, **cmd;
	
	if( ( buf = ipc_readline( source ) ) )
	{
		cmd = irc_parse_line( buf );
		if( cmd )
			ipc_command_exec( data, cmd, ipc_child_commands );
	}
	else
	{
		gaim_input_remove( global.listen_watch_source_id );
		close( global.listen_socket );
		
		global.listen_socket = -1;
	}
}

void ipc_to_master( char **cmd )
{
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
	{
		char *s = irc_build_line( cmd );
		ipc_to_master_str( s );
		g_free( s );
	}
	else if( global.conf->runmode == RUNMODE_DAEMON )
	{
		ipc_command_exec( NULL, cmd, ipc_master_commands );
	}
}

void ipc_to_master_str( char *msg_buf )
{
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
	{
		write( global.listen_socket, msg_buf, strlen( msg_buf ) );
	}
	else if( global.conf->runmode == RUNMODE_DAEMON )
	{
		char *s, **cmd;
		
		/* irc_parse_line() wants a read-write string, so get it one: */
		s = g_strdup( msg_buf );
		cmd = irc_parse_line( s );
		
		ipc_command_exec( NULL, cmd, ipc_master_commands );
		
		g_free( cmd );
		g_free( s );
	}
}

void ipc_to_children( char **cmd )
{
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
	{
		char *msg_buf = irc_build_line( cmd );
		ipc_to_children_str( msg_buf );
		g_free( msg_buf );
	}
	else if( global.conf->runmode == RUNMODE_DAEMON )
	{
		GSList *l;
		
		for( l = irc_connection_list; l; l = l->next )
			ipc_command_exec( l->data, cmd, ipc_child_commands );
	}
}

void ipc_to_children_str( char *msg_buf )
{
	if( global.conf->runmode == RUNMODE_FORKDAEMON )
	{
		int msg_len = strlen( msg_buf );
		GSList *l;
		
		for( l = child_list; l; l = l->next )
		{
			struct bitlbee_child *c = l->data;
			write( c->ipc_fd, msg_buf, msg_len );
		}
	}
	else if( global.conf->runmode == RUNMODE_DAEMON )
	{
		char *s, **cmd;
		
		/* irc_parse_line() wants a read-write string, so get it one: */
		s = g_strdup( msg_buf );
		cmd = irc_parse_line( s );
		
		ipc_to_children( cmd );
		
		g_free( cmd );
		g_free( s );
	}
}
