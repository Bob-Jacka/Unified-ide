#ifdef KEYWORDS

#define KEYWORDS

//Extensions:

char *c_cpp_extensions[] = {".c",".h",".cpp",".hpp",".cc",NULL};

char *python_extensions[] = {".py"};

char *sys_verilog_extensions[] = {};

//Extensions

//Language keywords:

char *c_cpp_keywords[] = {
	/* C Keywords */
	"auto","break","case","continue","default","do","else","enum",
	"extern","for","goto","if","register","return","sizeof","static",
	"struct","switch","typedef","union","volatile","while","NULL",

	/* C++ Keywords */
	"alignas","alignof","and","and_eq","asm","bitand","bitor","class",
	"compl","constexpr","const_cast","deltype","delete","dynamic_cast",
	"explicit","export","false","friend","inline","mutable","namespace",
	"new","noexcept","not","not_eq","nullptr","operator","or","or_eq",
	"private","protected","public","reinterpret_cast","static_assert",
	"static_cast","template","this","thread_local","throw","true","try",
	"typeid","typename","virtual","xor","xor_eq",

	/* C types */
     "int|","long|","double|","float|","char|","unsigned|","signed|",
     "void|","short|","auto|","const|","bool|",NULL
};

char *sys_verilog_keywords[] = {
		//TODO
};

//available keywords in python language
char *python_keywords[] = {
		"for","if","in","def","class", "async", "return","None","try",
		"except","else","as", "from", "import", "is","not"
};

//Language keywords

typedef struct hlcolor {
    int r, g, b;
} hlcolor;

struct editor_syntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[2];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

struct editor_syntax HLDB[] = {
    {
        /* C / C++ */
        c_cpp_extensions,
        c_cpp_keywords,
        "//","/*","*/",
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    }
};

#endif
