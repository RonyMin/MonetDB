/*
 * The contents of this file are subject to the MonetDB Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at
 * http://monetdb.cwi.nl/Legal/MonetDBLicense-1.0.html
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is the Monet Database System.
 * 
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-2004 CWI.
 * All Rights Reserved.
 * 
 * Contributor(s):
 * 		Martin Kersten <Martin.Kersten@cwi.nl>
 * 		Peter Boncz <Peter.Boncz@cwi.nl>
 * 		Niels Nes <Niels.Nes@cwi.nl>
 * 		Stefan Manegold  <Stefan.Manegold@cwi.nl>
 */

#include        <monetdb_config.h>
#include	<stdio.h>
#include	<ctype.h>
#ifdef HAVE_UNISTD_H
#include	<unistd.h>
#endif

#include	"Mx.h"
#include	"MxFcnDef.h"
#include	"disclaimer.h"

#ifdef HAVE_GETOPT_H
# include <getopt.h>
#else
#ifndef HAVE_GETOPT
# include "getopt.c"
#else
# include "mygetopt.h"
#endif
#endif

#ifndef UNIX
extern	int	_trace;
#endif


#ifndef MX_CXX_SUFFIX
#define MX_CXX_SUFFIX "cc"
#endif

unsigned int	db_flag= 0x00;
int	archived;		/* set for archived portions */
int	mode= M_TEXT;
int	opt_column= 1;
int	opt_hide= NO_HIDE;
int	textmode= M_TEX;
int	bodymode= 0;	/* all should be shown */
char	*opt_code;
char    *defHideText=0;

int	mx_err= 0;
char * 	mx_file= 0;
int 	mx_line= 0;
int	mx_chp =0, mx_mod=0, mx_sec= 0;
int	condSP=0;
int	codeline = 0;
int 	noline  = 0;
int 	notouch = 0;
char * texDocStyle = 0;

#ifdef UNIX
#define	Main	main
#endif

int	Main(argc, argv)
int	argc;
char **	argv;
{
	int i,k;
#ifndef UNIX	
	_trace= 1;
#endif

	if( argc == 1) {
		Message("Usage: Mx <flags> <file>.mx");
		Message("\t-t\t\tProduce LaTeX document (default)");
		Message("\t-c\t\tExtract code");
		Message("\t-R <dir>\tSet target directory to <dir>)");
		Message("\t-S <style>\tSet LaTeX documentstyle to 'style'");
		Message("\t-s\t\tProduce nroff -ms document");
		Message("\t-1\t\tSingle column (default) ");
		Message("\t-2\t\tDouble column");
		Message("\t-H <n>\t\tSet hide level to 'n' (-H0 default)");
		Message("\t-d\t\tProduce a draft document");
		Message("\t-x <extension>\tExtract <extension> labelled code");
		Message("\t-w\t\tExtract HTML code");
		Message("\t-W\t\tExtract HTML code for inclusion");
		Message("\t-D <id>\t\tDefine macro 'id'");
		Message("\t-T <string>\tDefine default hide text <string>");
		Message("\t-l\t\tNo #line and alike statements");
		Message("\t-n\t\tNon changed files won't be touched");
		Message("\t-+\t\tTreat @c (C code) as @C (C++ code)");
	    	exit(1);
	}
	InitDef();
	OutputDir(".");
	InitIndex();

/* Preprocess the arguments.
 */
	while ((i = getopt(argc, argv, "stcC:x:Bwdg:D:R:S:H:12T:ln+")) != EOF) {
		switch (i) {
		case 's':
			textmode = M_MS;
			break;
		case 't':
			textmode = M_TEX;
			break;
		case 'c':
			mode = M_CODE;
			break;
		case 'C':
			disclaimer = 1;
			disclaimerfile = optarg;
			break;
		case 'x':	/* code can be extracted selectively */
			mode = M_CODE;
			addextension(optarg);
			break;
		case 'B': bodymode = 1; /* use for inclusion */
			break;
		case 'w':
			textmode = M_WWW; 
			break;
		case 'd':
			mode = M_DRAFT;
			break;
		case 'g':
			sscanf(optarg, "%x", &db_flag);
			break;
		case 'D': {
			Def *d;
			d = NwDef(Mxmacro, 0, 0, 0);
			d->d_cmd = StrDup(optarg);
			d->d_blk = NULL;
			break;
		}
		case 'R':	
			OutputDir(optarg);
			break;
		case 'S':
			texDocStyle=StrDup(optarg);
			break;
		case 'H':
			sscanf(optarg, "%d", &opt_hide);
			break;
		case '1':
			opt_column = 1;
			break;
		case '2':
			opt_column = 2;
			break;
		case 'T':
			defHideText = optarg; 
			break;
		case 'l':
			noline = 1;
			break;
		case 'n':
			notouch = 1;
			break;
		case '+':   
			k = 0;
			do {
				if (str2dir[k].code == Csrc)
					strcpy(str2dir[k].ext, MX_CXX_SUFFIX);
			} while (str2dir[k++].code != Nop);
			break;
		default:
			Error("Unknown flag:%c", i);
		}
	}


	for (i = optind; i < argc; i++)
		MakeDefs(argv[i]);

	if( mode & M_CODE )
		GenCode();
	if( mode & M_DRAFT )
		GenForm();
	
	exit(mx_err ? 1 : 0);
	return 1;
}

Directive str2dir[]={
    { "", Continue,"", },
    { "0",Index0,"", },
    { "1",Index1,"", },
    { "2",Index2,"", },
    { "3",Index3,"", },
    { "4",Index4,"", },
    { "5",Index5,"", },
    { "6",Index6,"", },
    { "7",Index7,"", },
    { "8",Index8,"", },
    { "9",Index9,"", },
    { "f",Ofile,"", },
    { "=",Mxmacro,"", },
    { "ifdef",Ifdef,"", },
    { "else",Ifndef,"", },
    { "endif",Endif,"", },
    { "a", Author,"", },
    { "v", Version,"", },
    { "t", Title,"", },
    { "d", Date,"", },
    { "*",Module,"", },
    { "+",Section,"", },
    { "-",Subsection,"", },
    { ".",Paragraph,"", },
    { "T",Qtex,"", },
    { "C",CCsrc,MX_CXX_SUFFIX, },
    { "i",Pimpl,"impl", },
    { "s",Pspec,"spec", },
    { "h",Cdef,"h", },
    { "c",Csrc,"c", },
    { "y",Cyacc,"y", },
    { "l",Clex,"l", },
    { "odl",ODLspec,"odl", },
    { "oql",OQLspec,"oql", },
    { "sql",SQL,"sql", },
    { "p",Prolog,"pl", },
    { "hs",Haskell,"hs", },
    { "m",Monet,"m", },
    { "mal",MALcode,"mal", },
    { "mil",MILcode,"mil", },
    { "w",HTML,"www", },
    { "java",Java,"java", },
    { "tcl",Tcl,"tcl", },
    { "Qnap",Qnap,"qnp", },
    { "pc",ProC,"pc", },
    { "sh",Shell,"", },
    { "fgr",fGrammar,"fgr", },
    { "mcr",Macro,"mcr", },
    { "xml",XML,"xml", },
    { "dtd",DTD,"dtd", },
    { "xsl",XSL,"xsl", },
    { "cfg",Config,"cfg", },
    { "swig",Swig,"i", },
    { "Y",CCyacc,"yy", },
    { "L",CClex,"ll", },
    { "bib",BibTeX,"bib", },
    { "{",InHide,"", },
    { "}",OutHide,"", },
    { "/",Comment,"", },
    { NULL,Nop,NULL, },
};

#define NUMEXTENS (sizeof(str2dir)/sizeof(Directive))

int extcnt=0;
CmdCode extens[NUMEXTENS];
void addextension(char *ext)
{
    extens[extcnt] = lookup(ext);
    if (extens[extcnt] > 0) {
	extcnt++;
    }
    else fprintf(stderr,"Invalid extension %s\n",ext);
}
	
int extract(CmdCode dir)
{
   int i=0;
   if(extcnt==0) return 1;
   for(;i!=extcnt;i++)
       if(extens[i]==dir) return 1;
   return 0;
}
