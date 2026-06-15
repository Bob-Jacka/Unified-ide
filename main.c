#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "keywords.h" //highlight keywords in various languages
#include <stdint.h>
#include <signal.h>

#define CTRL_KEY(k) ((k) & 0x1f)

#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8      /* Search match. */

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

#define VERSION "1.0.0"
#define TAB_STOP 8
#define TO_QUIT_TIMES 3

/*** data ***/

//buffer like entity (dynamic string)
struct abuf {
  char *b;
  int len;
};

//active editor keywords:
enum editorKey {
		KEY_NULL = 0,       /* NULL */
        TAB = 15,            /* Tab */
        ENTER = 28,         /* Enter */
        ESC = 1,           /* Escape */
        BACKSPACE =  14,   /* Backspace */
        
        ARROW_LEFT = 105,
        ARROW_RIGHT = 106,
        ARROW_UP = 103,
        ARROW_DOWN = 108,
        
        DEL_KEY = 111,
        HOME_KEY = 102,
        END_KEY = 107,
        PAGE_UP = 104,
        PAGE_DOWN = 109
};

typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_oc;          
} erow;

//editor configuration parameters
struct Editor_config {
    int cx,cy;  	/* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    int numrows;    /* Number of rows */
    int rawmode;    /* Is terminal raw mode enabled? */
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;    /* Current syntax highlight, or NULL. */
};

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[2];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

struct Editor_config E; //declare global config variable

static struct termios orig_termios; /* In order to restore at exit.*/

//Prototypes:
char *editor_prompt(const char *prompt, void (*callback)(const char *, int));
int editor_row_rx_2_cx(erow *row, int rx);
void editor_insert_row(int at, char *s, size_t len);

void editor_set_status_msg(const char *fmt, ...) {
	va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

//last message in editor live
void die_hard(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
  
	perror(s);
	exit(1);
}

/*** data ***/

/*** terminal ***/

int get_cur_position(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
		return -1;
	}
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) {
			break;
		}
		if (buf[i] == 'R') {
			break;
		}
		++i;
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') {
		return -1;
	}
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
		return -1;
	}
	return 0;
}

//termanal window size getter
int get_win_size(int ifd, int ofd, int *rows, int *cols) {
  struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        int orig_row, orig_col, retval;

        retval = get_cur_position(ifd,ofd,&orig_row,&orig_col);
        if (retval == -1) {
			goto failed_handle;
		}

        /* Go to right/bottom margin and get position. */
        if (write(ofd, "\x1b[999C\x1b[999B",12) != 12) {
			goto failed_handle;
		}
        retval = get_cur_position(ifd,ofd,rows,cols);
        if (retval == -1) {
			goto failed_handle;
		}

        /* Restore position. */
        char seq[32];
        snprintf(seq,32,"\x1b[%d;%dH",orig_row,orig_col);
        if (write(ofd,seq,strlen(seq)) == -1) {
			//TODO
        }
        return 0;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }

failed_handle:
    return -1;
}

//is given character separator
int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

int editor_row_has_open_comment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize-1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize-2] != '*' ||
                            row->render[row->rsize-1] != '/'))) return 1;
    return 0;
}

//update syntax highlight
void editor_update_syntax(erow *row) {
  row->hl = realloc(row->hl,row->rsize);
    memset(row->hl,HL_NORMAL,row->rsize);

    if (E.syntax == NULL) return; /* No syntax, everything is HL_NORMAL. */

    int i, prev_sep, in_string, in_comment;
    char *p;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    /* Point to the first non-space char. */
    p = row->render;
    i = 0; /* Current char offset */
    while(*p && isspace(*p)) {
        p++;
        i++;
    }
    prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
    in_string = 0; /* Are we inside "" or '' ? */
    in_comment = 0; /* Are we inside multi-line comment? */

    /* If the previous line has an open comment, this line starts
     * with an open comment state. */
    if (row->idx > 0 && editor_row_has_open_comment(&E.row[row->idx-1])) {
	    in_comment = 1;	
	}

    while(*p) {
        /* Handle // comments. */
        if (prev_sep && *p == scs[0] && *(p+1) == scs[1]) {
            /* From here to end is a comment */
            memset(row->hl+i,HL_COMMENT,row->size-i);
            return;
        }

        /* Handle multi line comments. */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p+1) == mce[1]) {
                row->hl[i+1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++; 
                i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p+1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i+1] = HL_MLCOMMENT;
            p += 2; i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }

        /* Handle "" and '' */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i+1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }

        /* Handle non printable chars. */
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle numbers */
        if ((isdigit(*p) && (prev_sep || row->hl[i-1] == HL_NUMBER)) ||
            (*p == '.' && i >0 && row->hl[i-1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle keywords and lib calls */
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                if (!memcmp(p,keywords[j],klen) &&
                    is_separator(*(p+klen)))
                {
                    /* Keyword */
                    memset(row->hl+i,kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue; /* We had a keyword match */
            }
        }

        /* Not special chars */
        prev_sep = is_separator(*p);
        p++; i++;
    }

    /* Propagate syntax change to the next row if the open commen
     * state changed. This may recursively affect all the following rows
     * in the file. */
    int oc = editor_row_has_open_comment(row);
    if (row->hl_oc != oc && row->idx+1 < E.numrows) {
		    editor_update_syntax(&E.row[row->idx+1]);
	}
    
    row->hl_oc = oc;
}

int editor_syntax_2_color(int hl) {
    switch(hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;     /* cyan */
    case HL_KEYWORD1: return 33;    /* yellow */
    case HL_KEYWORD2: return 32;    /* green */
    case HL_STRING: return 35;      /* magenta */
    case HL_NUMBER: return 31;      /* red */
    case HL_MATCH: return 34;      /* blue */
    default: return 37;             /* white */
    }
}

char *editor_rows_2_string(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; ++j) {
	  totlen += E.row[j].size + 1;
  }
    
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (int x = 0; x < E.numrows; ++x) {
    memcpy(p, E.row[x].chars, E.row[x].size);
    p += E.row[x].size;
    *p = '\n';
    ++p;
  }
  return buf;
}

void editor_update_row(erow *row) { 
  unsigned int tabs = 0, nonprint = 0;
    int j, idx;

    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    unsigned long long allocsize =
        (unsigned long long) row->size + tabs*8 + nonprint*9 + 1;
    if (allocsize > UINT32_MAX) {
        printf("Some line of the edited file is too long for Unified IDE\n");
        exit(1);
    }

    row->render = malloc(row->size + tabs*8 + nonprint*9 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while((idx+1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
	editor_update_syntax(row);
}

void editor_append_row(char *s, size_t len) {
	  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	  int at = E.numrows;
	  E.row[at].size = len;
	  E.row[at].chars = malloc(len + 1);
	  memcpy(E.row[at].chars, s, len);
	  E.row[at].chars[len] = '\0';
	  E.row[at].rsize = 0;
	  E.row[at].render = NULL;
	  editor_update_row(&E.row[at]);
	  ++E.numrows;
	  ++E.dirty;
}

//open file in editor for editing
int editor_open(char *filename) {
	FILE *fp;

    E.dirty = 0;
    free(E.filename);
    size_t fnlen = strlen(filename)+1;
    E.filename = malloc(fnlen);
    memcpy(E.filename,filename,fnlen);

    fp = fopen(filename,"r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line,&linecap,fp)) != -1) {
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) {
			line[--linelen] = '\0';
		}
        editor_insert_row(E.numrows,line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}

//save editor statement
void editor_save() {
	if (E.filename == NULL) {
		E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
		if (E.filename == NULL) {
			editor_set_status_msg("Save aborted");
			return;
		}
	}
	  int len;
	  char *buf = editor_rows_2_string(&len);
	  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	  if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
		  if (write(fd, buf, len) == len) {
			close(fd);
			free(buf);
			E.dirty = 0;
			editor_set_status_msg("%d bytes written to disk", len);
			return;
		  }
		}
		close(fd);
	  }
	  free(buf);
	  editor_set_status_msg("Can't save! I/O error: %s", strerror(errno));
}

int editor_row_rx_2_cx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; ++cx) {
    if (row->chars[cx] == '\t')
      cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
    ++cur_rx;
    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editor_find_callback(const char *query, int key) {
  static int last_match = -1;
  static int direction = 1;
  static int saved_hl_line;
  static char *saved_hl = NULL;
  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }
  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }
  if (last_match == -1) {
	  direction = 1;
  }
  int current = last_match;
  int i;
  for (i = 0; i < E.numrows; ++i) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;
    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editor_row_rx_2_cx(row, match - row->render);
      E.rowoff = E.numrows;
      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editor_find() {
int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;
  char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);
  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) {
	  return;
  }
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

void disable_raw_mode(int fd) {
  if (E.rawmode) {
        tcsetattr(fd,TCSAFLUSH,&orig_termios);
        E.rawmode = 0;
    }
}

void editor_at_exit(void) {
    disable_raw_mode(STDIN_FILENO);
}

int enable_raw_mode(int fd) {
	struct termios raw;

    if (E.rawmode) {
		return 0; /* Already enabled. */
	}
	
    if (!isatty(STDIN_FILENO)) {
		goto fatal_handle;
	}
	
    atexit(editor_at_exit);
    if (tcgetattr(fd, &orig_termios) == -1) {
		goto fatal_handle;
	}

    raw = orig_termios;  /* modify the original mode */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) {
		goto fatal_handle;
	}
    E.rawmode = 1;
    return 0;

fatal_handle:
    errno = ENOTTY;
    return -1;
}

void editor_scroll() {
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  }
  if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}

void editor_draw_rows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; ++y) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Editor -- version %s", VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          --padding;
        }
        while (--padding) {
			abAppend(ab, " ", 1);
		}
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      char *c = &E.row[filerow].render[E.coloff];
      unsigned char *hl = &E.row[filerow].hl[E.coloff];
      int current_color = -1;
      int j;
      for (j = 0; j < len; ++j) {
        if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editor_syntax_2_color(hl[j]);
          if (color != current_color) {
            current_color = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editor_draw_status_Bar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      ++len;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editor_draw_msg_bar(struct abuf *ab) {
	//TODO
}

int editor_row_cx_2_rx(erow *row, int cx) { 
	  return 0;
}

void editor_insert_row(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) {
	  return;
  }
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  editor_update_row(&E.row[at]);
  
  ++E.numrows;
  ++E.dirty;
}

void editor_free_row(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editor_del_row(int at) {
    erow *row;

    if (at >= E.numrows) {
		return;
	}
    row = E.row + at;
    editor_free_row(row);
    memmove(E.row+at,E.row+at+1,sizeof(E.row[0])*(E.numrows-at-1));
    for (int j = at; j < E.numrows-1; ++j) {
		E.row[j].idx++;
	}
    --E.numrows;
    ++E.dirty;
}

/* Delete the character at offset 'at' from the specified row. */
void editor_row_del_char(erow *row, int at) {
    if (row->size <= at) return;
    memmove(row->chars+at,row->chars+at+1,row->size-at);
    editor_update_row(row);
    row->size--;
    E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c) {
  if (at < 0 || at > row->size) {
	  at = row->size;
  }
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  ++row->size;
  row->chars[at] = c;
  editor_update_row(row);
  ++E.dirty;
}

void editor_insert_char(int c) {
  if (E.cy == E.numrows) {
    editor_append_row("", 0);
  }
  editor_row_insert_char(&E.row[E.cy], E.cx, c);
  ++E.cx;
}

#define ABUF_INIT {NULL,0}

//delete chars from screen
void editor_refresh_screen() {
int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;

    abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
    abAppend(&ab,"\x1b[H",3); /* Go home. */
    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff+y;

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows/3) {
                char welcome[80];
                int welcomelen = snprintf(welcome,sizeof(welcome),
                    "Editor -- verison %s\x1b[0K\r\n", VERSION);
                int padding = (E.screencols-welcomelen)/2;
                if (padding) {
                    abAppend(&ab,"~",1);
                    padding--;
                }
                while(padding--) abAppend(&ab," ",1);
                abAppend(&ab,welcome,welcomelen);
            } else {
                abAppend(&ab,"~\x1b[0K\r\n",7);
            }
            continue;
        }

        r = &E.row[filerow];

        int len = r->rsize - E.coloff;
        int current_color = -1;
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render+E.coloff;
            unsigned char *hl = r->hl+E.coloff;
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NONPRINT) {
                    char sym;
                    abAppend(&ab,"\x1b[7m",4);
                    if (c[j] <= 26)
                        sym = '@'+c[j];
                    else
                        sym = '?';
                    abAppend(&ab,&sym,1);
                    abAppend(&ab,"\x1b[0m",4);
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab,"\x1b[39m",5);
                        current_color = -1;
                    }
                    abAppend(&ab,c+j,1);
                } else {
                    int color = editor_syntax_2_color(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                        current_color = color;
                        abAppend(&ab,buf,clen);
                    }
                    abAppend(&ab,c+j,1);
                }
            }
        }
        abAppend(&ab,"\x1b[39m",5);
        abAppend(&ab,"\x1b[0K",4);
        abAppend(&ab,"\r\n",2);
    }

    /* Create a two rows status. First row: */
    abAppend(&ab,"\x1b[0K",4);
    abAppend(&ab,"\x1b[7m",4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",E.rowoff+E.cy+1,E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(&ab,status,len);
    while(len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab,rstatus,rlen);
            break;
        } else {
            abAppend(&ab," ",1);
            len++;
        }
    }
    abAppend(&ab,"\x1b[0m\r\n",6);

    /* Second row depends on E.statusmsg and the status message update time. */
    abAppend(&ab,"\x1b[0K",4);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL)-E.statusmsg_time < 5) {
		abAppend(&ab,E.statusmsg,msglen <= E.screencols ? msglen : E.screencols);
	}
    
    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    int j;
    int cx = 1;
    int filerow = E.rowoff+E.cy;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    if (row) {
        for (j = E.coloff; j < (E.cx+E.coloff); j++) {
            if (j < row->size && row->chars[j] == TAB) cx += 7-((cx)%8);
            cx++;
        }
    }
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy+1,cx);
    abAppend(&ab,buf,strlen(buf));
    abAppend(&ab,"\x1b[?25h",6); /* Show cursor. */
    write(STDOUT_FILENO,ab.b,ab.len);
    abFree(&ab);
}

/*** terminal ***/

char editor_read_key() {
  int nread; //read count
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
		die_hard("editor_read_key");
	}
  }
  return c;
}

/*** input ***/

char *editor_prompt(const char *prompt, void (*callback)(const char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editor_set_status_msg(prompt, buf);
    editor_refresh_screen();
    int c = editor_read_key();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) {
		  buf[--buflen] = '\0';
	  }
    } else if (c == '\x1b') {
      editor_set_status_msg("");
      if (callback) {
		  callback(buf, c);
	  }
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editor_set_status_msg("Buffer is zero");
        if (callback) {
			callback(buf, c);
		}
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback) {
		callback(buf, c);
	}
  }
}

//move cursor in editor
void editor_mv_cursor(int key) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch(key) {
		case ARROW_LEFT:
			if (E.cx == 0) {
				if (E.coloff) {
					E.coloff--;
				} else {
					if (filerow > 0) {
						E.cy--;
						E.cx = E.row[filerow-1].size;
						if (E.cx > E.screencols-1) {
							E.coloff = E.cx-E.screencols+1;
							E.cx = E.screencols-1;
						}
					}
				}
			} else {
				E.cx -= 1;
			}
			break;
        
		case ARROW_RIGHT:
			if (row && filecol < row->size) {
				if (E.cx == E.screencols-1) {
					E.coloff++;
				} else {
					E.cx += 1;
				}
			} else if (row && filecol == row->size) {
				E.cx = 0;
				E.coloff = 0;
				if (E.cy == E.screenrows-1) {
					E.rowoff++;
				} else {
					E.cy += 1;
				}
			}
			break;
			
		case ARROW_UP:
			if (E.cy == 0) {
				if (E.rowoff) E.rowoff--;
			} else {
				E.cy -= 1;
			}
			break;
			
		case ARROW_DOWN:
			if (filerow < E.numrows) {
				if (E.cy == E.screenrows-1) {
					E.rowoff++;
				} else {
					E.cy += 1;
				}
			}
			break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff+E.cy;
    filecol = E.coloff+E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol-rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

void editor_row_append_string(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars,row->size+len+1);
    memcpy(row->chars+row->size,s,len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    ++E.dirty;
}

//delete character from editor string
void editor_del_char(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        filecol = E.row[filerow-1].size;
        editor_row_append_string(&E.row[filerow-1],row->chars,row->size);
        editor_del_row(filerow);
        row = NULL;
        if (E.cy == 0) {
			--E.rowoff;
		} else {
			--E.cy;
		}
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols-E.cx)+1;
            E.cx -= shift;
            E.coloff += shift;
        }
    } else {
        editor_row_del_char(row,filecol-1);
        if (E.cx == 0 && E.coloff) {
			E.coloff--;
		} else {
			--E.cx;
		}
            
    }
    if (row) {
		editor_update_row(row);
	}
    ++E.dirty;
}

//main function to interact with editor
void editor_process_keypress() {
  static int quit_times = TO_QUIT_TIMES;
  int c = editor_read_key();
  switch (c) {
	case '\r':
      /* TODO */
      break;
      
	case CTRL_KEY('q'): //for exit functionality
      if (E.dirty && quit_times) {
            editor_set_status_msg("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
            --quit_times;
            return;
        }
        exit(0);
        break;
    
    case CTRL_KEY('s'): //for save functionality
		editor_save();
		break;
		
	case CTRL_KEY('f'): //for find functionality
		editor_find();
		break;
      
    case HOME_KEY:
      E.cx = 0;
      break;
      
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;
     
    //For deleting
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      editor_del_char();
      break;
      
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }
        int times = E.screenrows;
        while (--times)
          editor_mv_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
     }
     break;
      
    //For moving in editor with arrows
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_mv_cursor(c);
      break;
      
    case CTRL_KEY('l'):
        editor_refresh_screen();
        break;
      
    case ESC:
        break;
      
    default:
      editor_insert_char(c);
      break;
  }
  
  quit_times = TO_QUIT_TIMES;
}
/*** init ***/

void update_win_size() {
	    if (get_win_size(STDIN_FILENO,STDOUT_FILENO, &E.screenrows,&E.screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E.screenrows -= 2; /* Get room for status bar. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
    update_win_size();
    if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
    if (E.cx > E.screencols) E.cx = E.screencols - 1;
    editor_refresh_screen();
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    update_win_size();
    signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char *argv[]) {
	enable_raw_mode(STDIN_FILENO);
	init_editor();
	
	if (argc >= 2) {
		editor_open(argv[1]); //first (second) argument as a file to open
	}
	editor_set_status_msg("HELP: Ctrl-s = save | Ctrl-q = quit | Ctrl-f = find");
	
	//infinity cycle for your pleasure
	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}
	return 0;
}
/*** init ***/
