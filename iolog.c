#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#define _GNU_SOURCE
#define __USE_GNU
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define USE_TERMIOS	0
#if USE_TERMIOS
#	include <termios.h>
#endif

#ifndef TASK
#	define TASK	"/bin/bash"
#endif

#ifndef LOGFILE
#	define LOGFILE	"/tmp/bash.log.txt"
#endif

#define MAX_ZEROS	4

#define BUF_NONE	0
#define BUF_READ	1
#define BUF_WRITE	2

#define READ_END	0
#define WRITE_END	1

struct buf {
	int	fds[ 2 ];
	ssize_t	size;
	ssize_t	off;
	int	op;
	char	*b_name;
	char	b_sign;
	char	data[ 2048 ];
};

#define BUF_CHAR_STDIN	'<'
#define BUF_CHAR_STDOUT	'>'
#define BUF_CHAR_STDERR	'!'
#define BUF_CHAR_LOG	'='

static char		*log_file_name = LOGFILE;
static char		*task_path = TASK;
#if USE_TERMIOS
static struct termios	stdin_termios;
static int		termios_used;
#endif
static int		sigchild_caugh;

struct buf	in__b = { { -1, -1 }, 0, 0, 0, "stdin", BUF_CHAR_STDIN, { 0 } };
struct buf	out_b = { { -1, -1 }, 0, 0, 0, "stdout", BUF_CHAR_STDOUT, { 0 } };
struct buf	err_b = { { -1, -1 }, 0, 0, 0, "stderr", BUF_CHAR_STDERR, { 0 } };
struct buf	log_b = { { -1, -1 }, 0, 0, 0, "log", BUF_CHAR_LOG, { 0 } };


static void log_add( char cc, const char *buf, ssize_t len );

#ifdef _DEBUG
static void log_printf( char const *fmt, ... )
{
	va_list	ap;
	char	buf[ 1024 ];
	int	len;
	
	va_start( ap, fmt );
	len = vsnprintf( buf, sizeof(buf), fmt, ap );
	if ( len > 0 ) log_add( BUF_CHAR_LOG, buf, len );
	va_end( ap );
}
#else
#	define log_printf(f,...)
#endif


static void buf__close_read( struct buf *pbuf )
{
	assert( pbuf );
	if (  -1 != pbuf->fds[READ_END]  &&  STDIN_FILENO != pbuf->fds[READ_END]  ) {
		log_printf( " %s: close read end\n", pbuf->b_name );
		close( pbuf->fds[READ_END] );
	}
	pbuf->fds[READ_END] = -1;
}


static void buf__close_write( struct buf *pbuf )
{
	assert( pbuf );
	if ( -1 != pbuf->fds[WRITE_END] ) {
		int to_close = 1 != pbuf->fds[WRITE_END] && STDOUT_FILENO != pbuf->fds[WRITE_END] ? 1 : 0;

		if ( to_close ) log_printf( " %s: close write end\n", pbuf->b_name );
		if ( BUF_WRITE == pbuf->op && pbuf->off < pbuf->size ) {
			// flush when needs...
			write( pbuf->fds[WRITE_END], pbuf->data + pbuf->off, pbuf->size - pbuf->off );
			if ( BUF_CHAR_LOG == pbuf->b_sign ) write( pbuf->fds[WRITE_END], "\n", 1 );
		}
		if ( to_close ) close( pbuf->fds[WRITE_END] );
	}
	pbuf->fds[WRITE_END] = -1;
}


static void  buf__close( struct buf *pbuf )
{
	assert( pbuf );
	buf__close_read( pbuf );
	buf__close_write( pbuf );
}


static int buf__open_pipe( struct buf *pbuf )
{
	assert( pbuf );
	if ( pipe( pbuf->fds ) ) {
		perror( "pipe()" );
		return -1;
	}
	return 0;
}


static void buf__fdset( int *pm, fd_set *pins, fd_set *pouts, struct buf *pbuf )
{
	assert( pbuf );
	assert( pm );
	if ( BUF_READ == pbuf->op ) {
		if ( -1 != pbuf->fds[READ_END] ) {
			FD_SET( pbuf->fds[READ_END], pins );
			if ( *pm < pbuf->fds[READ_END] ) *pm = pbuf->fds[READ_END];
		}
	} else if ( BUF_WRITE == pbuf->op ) {
		if ( -1 != pbuf->fds[WRITE_END] ) {
			FD_SET( pbuf->fds[WRITE_END], pouts );
			if ( *pm < pbuf->fds[WRITE_END] ) *pm = pbuf->fds[WRITE_END];
		}
	}
}


static int buf__io( fd_set *pins, fd_set *pouts, struct buf *pbuf )
{
	ssize_t		s;
	int		ops = 0;

	assert( pbuf );
	if ( BUF_READ == pbuf->op ) {
		if ( NULL == pins || FD_ISSET( pbuf->fds[READ_END], pins ) ) {
			pbuf->off = 0;
			errno = 0;
			pbuf->size = read( pbuf->fds[READ_END], pbuf->data, sizeof(pbuf->data) );
			if ( pbuf->size > 0 ) {
				ops++;
				if ( BUF_CHAR_LOG != pbuf->b_sign ) {
					log_printf( " %s: was read %d bytes\n", pbuf->b_name, pbuf->size );
					
					// log stdin/stdout/stderr on input event
					log_add( pbuf->b_sign, pbuf->data, pbuf->size );
				}
				if ( -1 != pbuf->fds[WRITE_END] ) {
					pbuf->op = BUF_WRITE;
				} else {
					if ( BUF_CHAR_LOG != pbuf->b_sign ) log_printf( " write end was closed, ignore these data" );
					pbuf->size = 0;
				}
			} else {
				buf__close( pbuf );
				pbuf->op = BUF_NONE;
				if ( BUF_CHAR_LOG != pbuf->b_sign ) log_printf( " %s: eof or error on read end\n", pbuf->b_name );
			}
// 		} else {
// 			log_printf( " %s: wait buf_read\n", pbuf->b_name );
		}
	} else if ( BUF_WRITE == pbuf->op ) {
		if ( NULL == pouts || FD_ISSET( pbuf->fds[WRITE_END], pouts ) ) {
			if ( pbuf->size > pbuf->off ) {
				s = write( pbuf->fds[WRITE_END], pbuf->data + pbuf->off, pbuf->size - pbuf->off );
				if ( s > 0 ) {
					ops++;
					if ( BUF_CHAR_LOG != pbuf->b_sign ) {
						log_printf( " %s: written %d of %d bytes\n", pbuf->b_name, s, pbuf->size );
					}
					pbuf->off += s;
					if ( pbuf->size <= pbuf->off ) {
						pbuf->size = pbuf->off = 0;
						if ( -1 != pbuf->fds[READ_END] ) {
							if ( BUF_CHAR_LOG != pbuf->b_sign ) pbuf->op = BUF_READ; 
						} else {
							if ( BUF_CHAR_LOG != pbuf->b_sign ) log_printf( " read end is closed, so remain in write mode" );
						}
					}
				} else {
					buf__close( pbuf );
					pbuf->op = BUF_NONE;
					if ( BUF_CHAR_LOG != pbuf->b_sign ) log_printf( " %s: failure on write end\n", pbuf->b_name );
				}
// 			} else {
// 				// nothing to do
			}
		} else {
			log_printf( " %s: wait buf_write\n", pbuf->b_name );
		}
	}
	return ops;
}


static void buf__may_close( struct buf *pbuf )
{
	struct stat	st;
	
	assert( pbuf );
	if ( pbuf->fds[READ_END] != -1 && fstat( pbuf->fds[READ_END], &st ) ) buf__close_read( pbuf );
	if ( pbuf->fds[WRITE_END] != -1 && fstat( pbuf->fds[WRITE_END], &st ) ) buf__close_write( pbuf );
	if ( sigchild_caugh && BUF_CHAR_STDIN == pbuf->b_sign && pbuf->fds[READ_END] != -1 ) buf__close_read( pbuf );
}



static void close_all( void )
{
	buf__close( &in__b );
	buf__close( &out_b );
	buf__close( &err_b );
	buf__close( &log_b );
}


static void log_add_internal( const char *buf, ssize_t len )
{
	while ( len && log_b.size < sizeof(log_b.data) ) {
		register char	c = *buf++;

		--len;
		log_b.data[ log_b.size++ ] = c;
	}
}


static void log_add( char cc, const char *buf, ssize_t len )
{
	static char	log_c = '?';
	
	while ( len && log_b.size < sizeof(log_b.data) ) {
		char	c = *buf++;
		--len;

		if ( log_c != cc ) {
			char	b[4] = { '\n', cc, cc, ' ' };
			log_c = cc;
			log_add_internal( b, sizeof(b) );
		}
		if ( c == '\n' ) {
			log_c = '?';
		} else {
			log_b.data[ log_b.size++ ] = c;
		}
	}
}


static void child( int ac, char **av, char **env )
{
	int	in__copy, out_copy, err_copy;
	
	in__copy = dup( STDIN_FILENO ); 	buf__close_write( &in__b );	dup2( in__b.fds[READ_END], STDIN_FILENO );	// replace stdin with pipe end
	out_copy = dup( STDOUT_FILENO );	buf__close_read( &out_b );	dup2( out_b.fds[WRITE_END], STDOUT_FILENO );	// replace stdout with pipe end
	err_copy = dup( STDERR_FILENO );	buf__close_read( &err_b );	dup2( err_b.fds[WRITE_END], STDERR_FILENO );	// replace stderr with pipe end

	av[0] = task_path;
	execvpe( av[0], av, env );
	
	dup2( in__copy, STDIN_FILENO );
	dup2( out_copy, STDOUT_FILENO );
	dup2( err_copy, STDERR_FILENO );
	
	fprintf( stderr, "execvpe( \"%s\", argv, env ); failed with code 0x%X (%s)\n", log_file_name, errno, strerror( errno ) );
}


static void signal_handler( int sa )
{
	static char	*sa_names[] = {
		NULL,
		"SIGHUP", "SIGINT", "SIGQUIT", "SIGILL",
		"SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE",
		"SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2",
		"SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT",
		"SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP",
		"SIGTTIN", "SIGTTOU", "SIGURG", "SIGXCPU", 
		"SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH",
		"SIGIO", "SIGPWR", "SIGSYS"
	};
	char		s[ 64 ];
	unsigned	len;

	if ( sa < sizeof(sa_names)/sizeof(sa_names[0]) && sa_names[sa] ) {
		len = snprintf( s, sizeof(s), " signal=%s (%d)\n", sa_names[sa], sa );
	} else {
		len = snprintf( s, sizeof(s), " signal=%d\n", sa );
	}
	log_add( '*', s, len );
	if ( SIGCHLD == sa ) sigchild_caugh++;
}


static void parent( pid_t childpid )
{
	fd_set			ins;
	fd_set			outs;
	struct timeval		tv;
	int			M, S;
	int			code;
	pid_t			waitcode;
	struct sigaction	sa;
	
	buf__close_read( &in__b );	in__b.fds[READ_END] = STDIN_FILENO;	// do not use dup2( STDIN_FILENO, in__b.fds[READ_END] ); -- this does not work
	buf__close_write( &out_b );	out_b.fds[WRITE_END] = STDOUT_FILENO;	// do not use dup2( STDOUT_FILENO, out_b.fds[WRITE_END] ); -- this does not work
	buf__close_write( &err_b );	err_b.fds[WRITE_END] = STDERR_FILENO;	// do not use dup2( STDERR_FILENO, err_b.fds[WRITE_END] ); -- this does not work
	
	memset( &sa, 0, sizeof(sa) );
	sa.sa_handler = signal_handler;
	for ( S = SIGHUP; S <= SIGSYS; S++ ) sigaction( S, &sa, NULL );
	
#if USE_TERMIOS
	if ( !tcgetattr( STDIN_FILENO, &stdin_termios ) ) {
		struct termios  tcs;
		
		termios_used = 1;
		tcs = stdin_termios;
		tcs.c_lflag &= ~(ICANON | ISIG);
		errno = 0;
		if ( tcsetattr( STDIN_FILENO, TCSANOW, &tcs ) ) {
			log_add( '*', " tcsetattr failed\n", 18 );
		}
	} else {
		log_add( '*', " tcgetattr failed\n", 18 );
	}
#endif

	out_b.op = err_b.op = in__b.op = BUF_READ;
	log_b.op = BUF_WRITE;

	S = 0;
	waitcode = waitpid( childpid, &S, WNOHANG );
	log_printf( " startup waitid(%d)=%d; S=%X (ifexit=%X exitcode=%X)\n", (int)childpid, (int)waitcode, S, WIFEXITED( S ), WEXITSTATUS( S ) );
	for (;;) {
		M = -1;
		FD_ZERO( &ins );
		FD_ZERO( &outs );
		buf__fdset( &M, &ins, &outs, &in__b );
		buf__fdset( &M, &ins, &outs, &out_b );
		buf__fdset( &M, &ins, &outs, &err_b );
		buf__fdset( &M, &ins, &outs, &log_b );
		if ( -1 == M ) break;
		M++;
		
		tv.tv_sec = 0;
		tv.tv_usec = 1000;
		code = select( M, &ins, &outs, NULL, &tv );
		log_printf(
			" select=%d (in:%c%c out:%c%c  err:%c%c)\n",
			code,
			-1 != in__b.fds[READ_END] && FD_ISSET( in__b.fds[READ_END], &ins ) ? '+' : '-',
			-1 != in__b.fds[WRITE_END] && FD_ISSET( in__b.fds[WRITE_END], &outs ) ? '+' : '-',
			-1 != out_b.fds[READ_END] && FD_ISSET( out_b.fds[READ_END], &ins ) ? '+' : '-',
			-1 != out_b.fds[WRITE_END] && FD_ISSET( out_b.fds[WRITE_END], &outs ) ? '+' : '-',
			-1 != err_b.fds[READ_END] && FD_ISSET( err_b.fds[READ_END], &ins ) ? '+' : '-',
			-1 != err_b.fds[WRITE_END] && FD_ISSET( err_b.fds[WRITE_END], &outs ) ? '+' : '-'
		);
		if ( -1 == code ) {
			perror( "select()" );
			break;
		} else if ( 0 == code ) {
			// nothing... is some open?
			log_printf( " NEXT LOOP...\n" );
		} else {
			// process i/o
			int	o = 0;

			o += buf__io( &ins, &outs, &in__b );
			o += buf__io( &ins, &outs, &out_b );
			o += buf__io( &ins, &outs, &err_b );
			buf__io( &ins, &outs, &log_b );
			if ( !o ) usleep( 100000 );
		}
		
		buf__may_close( &in__b );
		buf__may_close( &out_b );
		buf__may_close( &err_b );
		
		S = 0;
		waitcode = waitpid( childpid, &S, WNOHANG );
		log_printf(
			" inloop waitid(%d)=%d; S=%X (ifexit=%X exitcode=%X) with in.op=%d/%d/%d out.op=%d/%d/%d err.op=%d/%d/%d\n",
			(int)childpid, (int)waitcode, S, WIFEXITED( S ), WEXITSTATUS( S ),
			in__b.op, in__b.fds[READ_END], in__b.fds[WRITE_END],
			out_b.op, out_b.fds[READ_END], out_b.fds[WRITE_END],
			err_b.op, err_b.fds[READ_END], err_b.fds[WRITE_END]
		);

		if (
			!(
				( BUF_WRITE == in__b.op && -1 != in__b.fds[WRITE_END] )
				||
				( BUF_READ == in__b.op && -1 != in__b.fds[READ_END] )
				||
				( BUF_WRITE == out_b.op && -1 != out_b.fds[WRITE_END] )
				||
				( BUF_READ == out_b.op && -1 != out_b.fds[READ_END] )
				||
				( BUF_WRITE == err_b.op && -1 != err_b.fds[WRITE_END] )
				||
				( BUF_READ == err_b.op && -1 != err_b.fds[READ_END] )
				||
				( log_b.off < log_b.size && -1 != log_b.fds[WRITE_END] )
				||
				0 == waitcode
			)
		) break;
	}
	if ( !waitcode ) {
		S = 0;
		waitcode = waitpid( childpid, &S, 0 );
		log_printf( " last waitid(%d)=%d; S=%X (ifexit=%X exitcode=%X)\n", (int)childpid, (int)waitcode, S, WIFEXITED( S ), WEXITSTATUS( S ) );
	} else {
		log_printf( " break loop with waitid(%d)=%d; S=%X (ifexit=%X exitcode=%X)\n", (int)childpid, (int)waitcode, S, WIFEXITED( S ), WEXITSTATUS( S ) );
	}

	if ( WIFEXITED( S ) ) {
		S = WEXITSTATUS( S );
	} else if ( WIFSIGNALED( S ) ) {
		S = 128 + WTERMSIG( S );
	} else if ( WIFSTOPPED( S ) ) {
		S = 128 + WSTOPSIG( S );
	} else {
		S = 128;
	}
	log_printf( " exit code will be S=0x%X (%d.)\n", S, S );

	log_add_internal( "\n", 1 );
	buf__io( &ins, NULL, &log_b );

#if USE_TERMIOS
	if ( termios_used ) tcsetattr( STDIN_FILENO, TCSANOW, &stdin_termios );
#endif
	
	close_all();
	exit( S );
}


static void load_conf( char *prog )
{
	char	*conf_name;
	int	conf_file;
	char	*conf_text;
	off_t	len;
	char	*p, *pparam, *pval;
	
	conf_name = (char*)malloc( strlen( prog ) + 5 );
	if ( !conf_name ) return;

	strcpy( conf_name, prog );
	strcat( conf_name, ".conf" );
	
	conf_file = open( conf_name, O_RDONLY, 0 );
	if ( -1 == conf_file ) {
		free( conf_name );
		return;
	}
	
	len = lseek( conf_file, 0, SEEK_END );
	if ( (off_t)-1 == len ) {
		close( conf_file );
		free( conf_name );
		return;
	}
	
	conf_text = (char*)malloc( len+1 );
	if ( !conf_text ) {
		close( conf_file );
		free( conf_name );
		return;
	}

	lseek( conf_file, 0, SEEK_SET );
	read( conf_file, conf_text, len );
	conf_text[ len ] = '\0';
	
	for ( p = conf_text; p - conf_text < len; ) {
		register char	c = *p++;
		
		if ( '\0' <= c && c <= ' ' ) continue;
		if ( '#' == c ) {
   eol:
			while ( p - conf_text < len ) {
				c = *p++;
				if ( '\r' == c || '\n' == c ) break;
			}
			continue;
		}
		
		pparam = p - 1;
		while ( isalnum( c ) && p - conf_text < len ) c = *p++;
		*(p-1) = '\0';
		while ( isblank( c ) && p - conf_text < len ) c = *p++;
		if ( '=' != c ) {
			// wrong... skip upto eol
			fprintf( stderr, "Wrong syntax in \"%s\" after \"%s\"\n", conf_name, pparam );
			goto eol;
		}
		c = *p++;
		while ( isblank( c ) && p - conf_text < len ) c = *p++;
		pval = p - 1;
		while ( p - conf_text < len ) {
			c = *p++;
			if ( '\r' == c || '\n' == c ) {
				*(p-1) = '\0';
				break;
			}
		}

		if ( !strcasecmp( pparam, "logfile" ) ) {
			log_file_name = strdup( pval );
		} else if ( !strcasecmp( pparam, "exec" ) ) {
			task_path = strdup( pval );
		} else {
			fprintf( stderr, "Wrong option \"%s\" = \"%s\" in \"%s\"\n", pparam, pval, conf_name );
		}
	}

	free( conf_text );
	close( conf_file );
	free( conf_name );
}


int main( int ac, char **av, char **env )
{
	pid_t	x;

	load_conf( av[0] );

	unlink( log_file_name );
	log_b.fds[WRITE_END] = open( log_file_name, O_WRONLY|O_CREAT, 0644 );
	if ( -1 == log_b.fds[WRITE_END] ) {
		fprintf( stderr, "open( \"%s\", O_WRONLY|O_APPEND|O_CREAT, 0644 ); failed with code 0x%X (%s)\n", log_file_name, errno, strerror( errno ) );
	} else {
		int	i;
		log_add( BUF_CHAR_LOG, task_path, strlen(task_path) );
		for ( i = 1; i < ac; i++ ) {
			log_add( BUF_CHAR_LOG, " ", 1 );
			log_add( BUF_CHAR_LOG, av[i], strlen(av[i]) );
		}
		log_add( BUF_CHAR_LOG, "\n\n", strlen("\n\n") );

		if ( !buf__open_pipe( &in__b ) ) {
			if ( !buf__open_pipe( &out_b ) ) {
				if ( !buf__open_pipe( &err_b ) ) {
					
					x = fork();
					switch ( x ) {
					case -1:	// error
						perror( "fork()" );
						break;

					case 0:		// child
						child( ac, av, env );
						break;
						
					default:	// parent
						parent( x );
						break;
					}
				}
			}
		}
	}
	close_all();
	return errno;
}

//
// TODO: ctrl-D closes iolog's stdin, so it closes stdin of child ... etc
// for example: iolog runs bash; then we enter 'cat' so bash runs cat; then we press ctrl-D ... ughhh, both cat and bash will be terminated by iolog... :(
// !!
// ctrl-d flush line, so this is 0-bytes read operation -- I'm suppose EOF on read zero bytes...
//
// to solve this trouble pseudo-terminals must be used...
//
// or ... we may suppose "closed stdin - closed all"
//
//
