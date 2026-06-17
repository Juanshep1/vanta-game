/* Vanta SDL runtime: the same Value runtime + graphics as the V-NOx kernel,
   but hosted on a real OS via SDL2 (window, input, timing). Compiled together
   with a `vc -k` emitted game (which provides kmain()). No Python at runtime. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdint.h>
#include <mach-o/getsect.h>
#include <mach-o/dyld.h>
typedef unsigned int u32; typedef unsigned char u8;
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_arg __builtin_va_arg
#define va_end __builtin_va_end

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static int scmp(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (u8)*a-(u8)*b; }
static char* scpy(char* d,const char* s){ char* r=d; while((*d++=*s++)){} return r; }
static char* scat(char* d,const char* s){ char* r=d; while(*d)d++; while((*d++=*s++)){} return r; }
static char* sstr(const char* h,const char* n){ unsigned long nl=slen(n); if(!nl)return (char*)h; for(;*h;h++){ unsigned long i=0; while(i<nl&&h[i]==n[i])i++; if(i==nl)return (char*)h; } return 0; }
/* ---- conservative mark-sweep GC: game state never corrupts AND memory stays bounded ---- */
typedef struct Block { struct Block* next; size_t size; int mark; } Block;
static Block* g_head=0; static size_t g_live=0, g_next_gc=4u*1024*1024;
static void* g_stack_bottom=0; static int g_ingc=0;
static void** g_arr=0; static size_t g_n=0;
static Block** g_work=0; static size_t g_wcap=0, g_wn=0;
static int cmp_ptr(const void* a,const void* b){ void* x=*(void* const*)a,*y=*(void* const*)b; return (x<y)?-1:((x>y)?1:0); }
static int is_block(void* p){ size_t lo=0,hi=g_n; while(lo<hi){ size_t mid=(lo+hi)>>1; if(g_arr[mid]==p)return 1; if(g_arr[mid]<p)lo=mid+1; else hi=mid; } return 0; }
static void mark_push(Block* b){ if(b->mark)return; b->mark=1; if(g_wn>=g_wcap){ g_wcap=g_wcap?g_wcap*2:4096; g_work=(Block**)realloc(g_work,g_wcap*sizeof(Block*)); } g_work[g_wn++]=b; }
static void mark_range(void* lo,void* hi){ char* w=(char*)lo; for(; w+sizeof(void*)<=(char*)hi; w+=sizeof(void*)){ void* p=*(void**)w; if(is_block(p)) mark_push((Block*)p-1); } }
static void mark_segment(const char* seg,const char* sect){
  unsigned long sz=0; uint8_t* p=getsectiondata((const struct mach_header_64*)_dyld_get_image_header(0),seg,sect,&sz);
  if(p&&sz) mark_range(p,p+sz);   /* runtime, slide-adjusted address of the globals */
}
static void gc_run(void){
  size_t n=0; for(Block* b=g_head;b;b=b->next){ b->mark=0; n++; }
  g_arr=(void**)malloc((n?n:1)*sizeof(void*)); g_n=0;
  for(Block* b=g_head;b;b=b->next) g_arr[g_n++]=(void*)(b+1);
  qsort(g_arr,g_n,sizeof(void*),cmp_ptr);
  g_wn=0;
  jmp_buf jb; setjmp(jb); mark_range(&jb,(char*)&jb+sizeof(jb));   /* spilled registers */
  void* tmp; void* top=&tmp;
  if(top<g_stack_bottom) mark_range(top,g_stack_bottom); else mark_range(g_stack_bottom,top);  /* the C stack */
  mark_segment("__DATA","__data"); mark_segment("__DATA","__bss"); mark_segment("__DATA","__common");  /* globals = game state */
  while(g_wn){ Block* b=g_work[--g_wn]; void** w=(void**)(b+1); size_t cnt=b->size/sizeof(void*); for(size_t i=0;i<cnt;i++){ if(is_block(w[i])) mark_push((Block*)w[i]-1); } }
  Block** pp=&g_head; g_live=0;
  while(*pp){ Block* b=*pp; if(b->mark){ g_live+=b->size+sizeof(Block); pp=&b->next; } else { *pp=b->next; free(b); } }
  free(g_arr); g_arr=0; g_n=0;
  g_next_gc=g_live*2+2u*1024*1024;
}
static void* galloc(long n){
  if(!g_ingc && g_stack_bottom && g_live>g_next_gc){ g_ingc=1; gc_run(); g_ingc=0; }
  size_t sz=(size_t)(n>0?n:1);
  Block* b=(Block*)malloc(sizeof(Block)+sz);
  b->size=sz; b->mark=0; b->next=g_head; g_head=b; g_live+=sz+sizeof(Block);
  return (void*)(b+1);
}
static char* numstr(long v){ char t[32]; int i=0,neg=v<0; unsigned long u=neg?(unsigned long)(-v):(unsigned long)v; if(!u)t[i++]='0'; while(u){t[i++]=(char)('0'+u%10);u/=10;} char* b=galloc(i+neg+1); int j=0; if(neg)b[j++]='-'; while(i)b[j++]=t[--i]; b[j]=0; return b; }

enum { TN,TS,TB,TL,TM,TX };
typedef struct Value Value; typedef struct { Value* items; long len,cap; } List; typedef struct { char** keys; Value* vals; long len,cap; } Map;
struct Value { int t; long n; char* s; List* l; Map* m; };
static Value NUM(long n){ Value v; v.t=TN; v.n=n; v.s=0;v.l=0;v.m=0; return v; }
static Value BOOLV(int b){ Value v=NUM(b?1:0); v.t=TB; return v; }
static Value NIL(void){ Value v=NUM(0); v.t=TX; return v; }
static Value STR(const char* s){ Value v; v.t=TS; if(!s)s=""; char* r=galloc(slen(s)+1); scpy(r,s); v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static char* tostr(Value v){ if(v.t==TS)return v.s; if(v.t==TN)return numstr(v.n); if(v.t==TB)return v.n?"yes":"no"; if(v.t==TX)return "nothing"; if(v.t==TL){ char* o=galloc(8192); o[0]=0; scat(o,"["); for(long i=0;i<v.l->len;i++){if(i)scat(o,", ");scat(o,tostr(v.l->items[i]));} scat(o,"]"); return o;} if(v.t==TM){ char* o=galloc(8192); o[0]=0; scat(o,"{"); for(long i=0;i<v.m->len;i++){if(i)scat(o,", ");scat(o,v.m->keys[i]);scat(o,": ");scat(o,tostr(v.m->vals[i]));} scat(o,"}"); return o;} return ""; }
static int truthy(Value v){ if(v.t==TX)return 0; if(v.t==TB)return v.n!=0; return 1; }
static int veq(Value a,Value b){ if((a.t==TN||a.t==TB)&&(b.t==TN||b.t==TB))return a.n==b.n; if(a.t!=b.t)return 0; if(a.t==TS)return scmp(a.s,b.s)==0; if(a.t==TX)return 1; return 0; }
static Value ADD(Value a,Value b){ if(a.t==TN&&b.t==TN)return NUM(a.n+b.n); char* x=tostr(a); char* y=tostr(b); char* r=galloc(slen(x)+slen(y)+1); scpy(r,x); scat(r,y); Value v; v.t=TS; v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static Value SUB(Value a,Value b){ return NUM(a.n-b.n); } static Value MUL(Value a,Value b){ return NUM(a.n*b.n); } static Value DIVV(Value a,Value b){ return NUM(b.n?a.n/b.n:0); } static Value NEG(Value a){ return NUM(-a.n); }
static Value EQ(Value a,Value b){ return BOOLV(veq(a,b)); } static Value NE(Value a,Value b){ return BOOLV(!veq(a,b)); }
static Value LT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<0); return BOOLV(a.n<b.n); } static Value GT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>0); return BOOLV(a.n>b.n); }
static Value LE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<=0); return BOOLV(a.n<=b.n); } static Value GE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>=0); return BOOLV(a.n>=b.n); }
static Value ANDV(Value a,Value b){ return BOOLV(truthy(a)&&truthy(b)); } static Value ORV(Value a,Value b){ return BOOLV(truthy(a)||truthy(b)); } static Value NOTV(Value a){ return BOOLV(!truthy(a)); }
static List* newlist(void){ List* l=galloc(sizeof(List)); l->len=0;l->cap=8;l->items=galloc(sizeof(Value)*8); return l; }
static Value LIST0(void){ Value v; v.t=TL; v.l=newlist(); v.s=0;v.m=0;v.n=0; return v; }
static void listpush(Value lv,Value x){ List* l=lv.l; if(l->len>=l->cap){ long nc=l->cap*2; Value* ni=galloc(sizeof(Value)*nc); memcpy(ni,l->items,sizeof(Value)*l->len); l->items=ni; l->cap=nc; } l->items[l->len++]=x; }
static Value MKLIST(int n,...){ Value v=LIST0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++)listpush(v,va_arg(ap,Value)); va_end(ap); return v; }
static Map* newmap(void){ Map* m=galloc(sizeof(Map)); m->len=0;m->cap=8;m->keys=galloc(sizeof(char*)*8);m->vals=galloc(sizeof(Value)*8); return m; }
static Value MAP0(void){ Value v; v.t=TM; v.m=newmap(); v.s=0;v.l=0;v.n=0; return v; }
static void mapset(Value mv,Value k,Value val){ Map* m=mv.m; char* key=tostr(k); for(long i=0;i<m->len;i++)if(scmp(m->keys[i],key)==0){m->vals[i]=val;return;} if(m->len>=m->cap){long nc=m->cap*2;char** nk=galloc(sizeof(char*)*nc);Value* nv=galloc(sizeof(Value)*nc);memcpy(nk,m->keys,sizeof(char*)*m->len);memcpy(nv,m->vals,sizeof(Value)*m->len);m->keys=nk;m->vals=nv;m->cap=nc;} char* kc=galloc(slen(key)+1); scpy(kc,key); m->keys[m->len]=kc; m->vals[m->len]=val; m->len++; }
static Value MKMAP(int n,...){ Value v=MAP0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++){Value k=va_arg(ap,Value);Value val=va_arg(ap,Value);mapset(v,k,val);} va_end(ap); return v; }
static Value INDEX(Value c,Value k){ if(c.t==TL){long i=k.n;if(i<0)i+=c.l->len;if(i<0||i>=c.l->len)return NIL();return c.l->items[i];} if(c.t==TM){char* key=tostr(k);for(long i=0;i<c.m->len;i++)if(scmp(c.m->keys[i],key)==0)return c.m->vals[i];return NIL();} if(c.t==TS){long L=slen(c.s);long i=k.n;if(i<0)i+=L;if(i<0||i>=L)return STR("");char b[2]={c.s[i],0};return STR(b);} return NIL(); }
static void SETAT(Value c,Value k,Value val){ if(c.t==TL){long i=k.n;if(i>=0&&i<c.l->len)c.l->items[i]=val;} else if(c.t==TM)mapset(c,k,val); }
static Value SLICE(Value c,Value a,Value b){ long lo=a.n,hi=b.n; if(c.t==TS){long L=slen(c.s);if(lo<0)lo+=L;if(hi<0)hi+=L;if(lo<0)lo=0;if(hi>L)hi=L;if(hi<lo)hi=lo;char* r=galloc(hi-lo+1);memcpy(r,c.s+lo,hi-lo);r[hi-lo]=0;return STR(r);} if(c.t==TL){Value v=LIST0();long L=c.l->len;if(lo<0)lo+=L;if(hi<0)hi+=L;if(lo<0)lo=0;if(hi>L)hi=L;for(long i=lo;i<hi;i++)listpush(v,c.l->items[i]);return v;} return NIL(); }
static Value LEN(Value v){ if(v.t==TS)return NUM(slen(v.s)); if(v.t==TL)return NUM(v.l->len); if(v.t==TM)return NUM(v.m->len); return NUM(0); }
static Value INOP(Value a,Value b){ if(b.t==TS&&a.t==TS)return BOOLV(sstr(b.s,a.s)!=0); if(b.t==TL){for(long i=0;i<b.l->len;i++)if(veq(a,b.l->items[i]))return BOOLV(1);return BOOLV(0);} return BOOLV(0); }
static Value B_text(Value a){ return STR(tostr(a)); } static Value B_length(Value a){ return LEN(a); }
static Value B_range(Value a,Value b,int two){ Value v=LIST0(); long lo=two?a.n:0,hi=two?b.n:a.n; for(long i=lo;i<hi;i++)listpush(v,NUM(i)); return v; }
static Value B_join(Value lst,Value sep){ if(lst.t!=TL)return STR(""); char* d=tostr(sep); char* o=galloc(8192); o[0]=0; for(long i=0;i<lst.l->len;i++){if(i)scat(o,d);scat(o,tostr(lst.l->items[i]));} return STR(o); }
static Value B_upper(Value a){ char* s=tostr(a); char* r=galloc(slen(s)+1); long i=0; for(;s[i];i++){char c=s[i];r[i]=(c>='a'&&c<='z')?c-32:c;} r[i]=0; return STR(r); }
static Value B_sort(Value lst){ if(lst.t!=TL)return lst; Value v=LIST0(); for(long i=0;i<lst.l->len;i++)listpush(v,lst.l->items[i]); for(long i=1;i<v.l->len;i++){Value key=v.l->items[i];long j=i-1;while(j>=0&&truthy(GT(v.l->items[j],key))){v.l->items[j+1]=v.l->items[j];j--;}v.l->items[j+1]=key;} return v; }
static Value B_slice(Value c,Value a,Value b){ return SLICE(c,a,b); }
static Value B_lower(Value a){ char* s=tostr(a); char* r=galloc(slen(s)+1); long i=0; for(;s[i];i++){char c=s[i];r[i]=(c>='A'&&c<='Z')?c+32:c;} r[i]=0; return STR(r); }
static Value B_keys(Value m){ Value v=LIST0(); if(m.t==TM)for(long i=0;i<m.m->len;i++)listpush(v,STR(m.m->keys[i])); return v; }
static Value B_abs(Value a){ return NUM(a.n<0?-a.n:a.n); }

/* ===================== graphics (draw into a BACK buffer) ===================== */
static u32 SW=900, SH=600; static u32* BACK; static u32* WALL;
static inline void putpx(int x,int y,u32 c){ if((unsigned)x>=SW||(unsigned)y>=SH)return; BACK[y*SW+x]=c; }
static void fillrect(int x,int y,int w,int h,u32 c){ if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} for(int j=0;j<h;j++)for(int i=0;i<w;i++)putpx(x+i,y+j,c); }
static const u8 FONT[95][8]={
{0,0,0,0,0,0,0,0},{24,24,24,24,0,0,24,0},{54,54,0,0,0,0,0,0},{54,54,127,54,127,54,54,0},{12,63,3,30,48,31,12,0},{0,99,51,24,12,102,99,0},{28,54,28,110,59,51,110,0},{6,6,3,0,0,0,0,0},
{24,12,6,6,6,12,24,0},{6,12,24,24,24,12,6,0},{0,102,60,255,60,102,0,0},{0,12,12,63,12,12,0,0},{0,0,0,0,0,12,12,6},{0,0,0,63,0,0,0,0},{0,0,0,0,0,12,12,0},{96,48,24,12,6,3,1,0},
{62,99,115,123,111,103,62,0},{12,14,12,12,12,12,63,0},{30,51,48,28,6,51,63,0},{30,51,48,28,48,51,30,0},{56,60,54,51,127,48,120,0},{63,3,31,48,48,51,30,0},{28,6,3,31,51,51,30,0},{63,51,48,24,12,12,12,0},
{30,51,51,30,51,51,30,0},{30,51,51,62,48,24,14,0},{0,12,12,0,0,12,12,0},{0,12,12,0,0,12,12,6},{24,12,6,3,6,12,24,0},{0,0,63,0,0,63,0,0},{6,12,24,48,24,12,6,0},{30,51,48,24,12,0,12,0},
{62,99,123,123,123,3,30,0},{12,30,51,51,63,51,51,0},{63,102,102,62,102,102,63,0},{60,102,3,3,3,102,60,0},{31,54,102,102,102,54,31,0},{127,70,22,30,22,70,127,0},{127,70,22,30,22,6,15,0},{60,102,3,3,115,102,124,0},
{51,51,51,63,51,51,51,0},{30,12,12,12,12,12,30,0},{120,48,48,48,51,51,30,0},{103,102,54,30,54,102,103,0},{15,6,6,6,70,102,127,0},{99,119,127,127,107,99,99,0},{99,103,111,123,115,99,99,0},{28,54,99,99,99,54,28,0},
{63,102,102,62,6,6,15,0},{30,51,51,51,59,30,56,0},{63,102,102,62,54,102,103,0},{30,51,7,14,56,51,30,0},{63,45,12,12,12,12,30,0},{51,51,51,51,51,51,63,0},{51,51,51,51,51,30,12,0},{99,99,99,107,127,119,99,0},
{99,99,54,28,28,54,99,0},{51,51,51,30,12,12,30,0},{127,99,49,24,76,102,127,0},{30,6,6,6,6,6,30,0},{3,6,12,24,48,96,64,0},{30,24,24,24,24,24,30,0},{8,28,54,99,0,0,0,0},{0,0,0,0,0,0,0,255},
{12,12,24,0,0,0,0,0},{0,0,30,48,62,51,110,0},{7,6,6,62,102,102,59,0},{0,0,30,51,3,51,30,0},{56,48,48,62,51,51,110,0},{0,0,30,51,63,3,30,0},{28,54,6,15,6,6,15,0},{0,0,110,51,51,62,48,31},
{7,6,54,110,102,102,103,0},{12,0,14,12,12,12,30,0},{48,0,48,48,48,51,51,30},{7,6,102,54,30,54,103,0},{14,12,12,12,12,12,30,0},{0,0,51,127,127,107,99,0},{0,0,31,51,51,51,51,0},{0,0,30,51,51,51,30,0},
{0,0,59,102,102,62,6,15},{0,0,110,51,51,62,48,120},{0,0,59,110,102,6,15,0},{0,0,62,3,30,48,31,0},{8,12,62,12,12,44,24,0},{0,0,51,51,51,51,110,0},{0,0,51,51,51,30,12,0},{0,0,99,107,127,127,54,0},
{0,0,99,54,28,54,99,0},{0,0,51,51,51,62,48,31},{0,0,63,25,12,38,63,0},{56,12,12,7,12,12,56,0},{24,24,24,0,24,24,24,0},{7,12,12,56,12,12,7,0},{110,59,0,0,0,0,0,0}};
static void drawchar(int x,int y,char c,u32 col,int sc){ if(c<32||c>126)return; const u8* g=FONT[c-32]; for(int r=0;r<8;r++){u8 b=g[r];for(int i=0;i<8;i++)if(b&(1<<i))fillrect(x+i*sc,y+r*sc,sc,sc,col);} }
static void drawtext(int x,int y,const char* s,u32 col,int sc){ int cx=x; while(*s){ if(*s=='\n'){y+=8*sc+3;cx=x;} else {drawchar(cx,y,*s,col,sc);cx+=8*sc;} s++; } }
static inline u32 blendpx(u32 bg,u32 fg,int a){ if(a<0)a=0; if(a>255)a=255; int ia=255-a; u32 r=((((fg>>16)&255)*a)+(((bg>>16)&255)*ia))/255; u32 g=((((fg>>8)&255)*a)+(((bg>>8)&255)*ia))/255; u32 b=(((fg&255)*a)+((bg&255)*ia))/255; return (r<<16)|(g<<8)|b; }
static inline void blendpx_at(int x,int y,u32 fg,int a){ if((unsigned)x>=SW||(unsigned)y>=SH)return; u32* p=&BACK[y*SW+x]; *p=blendpx(*p,fg,a); }
static inline u32 lerpc(u32 a,u32 b,int t,int m){ if(m<=0)m=1; if(t<0)t=0; if(t>m)t=m; int ar=(a>>16)&255,ag=(a>>8)&255,ab=a&255,br=(b>>16)&255,bg=(b>>8)&255,bb=b&255; int r=ar+(br-ar)*t/m,g=ag+(bg-ag)*t/m,bl=ab+(bb-ab)*t/m; return ((u32)r<<16)|((u32)g<<8)|(u32)bl; }
static int in_round(int i,int j,int w,int h,int r){ int cx,cy; if(i<r&&j<r){cx=r;cy=r;} else if(i>=w-r&&j<r){cx=w-r-1;cy=r;} else if(i<r&&j>=h-r){cx=r;cy=h-r-1;} else if(i>=w-r&&j>=h-r){cx=w-r-1;cy=h-r-1;} else return 1; int dx=i-cx,dy=j-cy; return dx*dx+dy*dy<=r*r; }

/* ===================== SDL window + input ===================== */
static SDL_Window* g_win; static SDL_Renderer* g_ren; static SDL_Texture* g_tex; static int g_quit=0;
static int mx,my,mbtn; static char kbuf=0; static Uint8 g_prev[512];

Value v_poll(void){ memcpy(g_prev, SDL_GetKeyboardState(0), 512); SDL_Event e; while(SDL_PollEvent(&e)){
    if(e.type==SDL_QUIT) g_quit=1;
    else if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_RETURN)kbuf='\n'; else if(k==SDLK_BACKSPACE)kbuf='\b'; else if(k>=32&&k<127)kbuf=(char)k; }
    else if(e.type==SDL_MOUSEMOTION){ mx=e.motion.x; my=e.motion.y; }
    else if(e.type==SDL_MOUSEBUTTONDOWN){ mbtn=1; }
    else if(e.type==SDL_MOUSEBUTTONUP){ mbtn=0; }
  } return NIL(); }
Value v_key(void){ if(kbuf){char b[2]={kbuf,0}; kbuf=0; return STR(b);} return STR(""); }
Value v_held(Value name){ const char* n=tostr(name); const Uint8* st=SDL_GetKeyboardState(0); SDL_Scancode sc=SDL_SCANCODE_UNKNOWN;
  if(!scmp(n,"left"))sc=SDL_SCANCODE_LEFT; else if(!scmp(n,"right"))sc=SDL_SCANCODE_RIGHT; else if(!scmp(n,"up"))sc=SDL_SCANCODE_UP; else if(!scmp(n,"down"))sc=SDL_SCANCODE_DOWN;
  else if(!scmp(n,"space"))sc=SDL_SCANCODE_SPACE; else if(!scmp(n,"escape"))sc=SDL_SCANCODE_ESCAPE; else if(slen(n)==1){char c=n[0]; if(c>='a'&&c<='z')sc=(SDL_Scancode)(SDL_SCANCODE_A+(c-'a'));}
  if(sc==SDL_SCANCODE_UNKNOWN)return NUM(0); return NUM(st[sc]?1:0); }
Value v_quit(void){ return NUM(g_quit); }
Value v_ticks(void){ return NUM((long)SDL_GetTicks()); }
Value v_delay(Value ms){ SDL_Delay((Uint32)(ms.n<0?0:ms.n)); return NIL(); }
Value v_title(Value s){ if(g_win)SDL_SetWindowTitle(g_win,tostr(s)); return NIL(); }
Value v_mouse_x(void){ return NUM(mx); } Value v_mouse_y(void){ return NUM(my); } Value v_mouse_down(void){ return NUM(mbtn); }
Value v_screen_w(void){ return NUM(SW); } Value v_screen_h(void){ return NUM(SH); }

Value v_rgb(Value r,Value g,Value b){ return NUM((long)(((u32)r.n<<16)|((u32)g.n<<8)|(u32)b.n)); }
Value v_fill(Value x,Value y,Value w,Value h,Value c){ fillrect(x.n,y.n,w.n,h.n,(u32)c.n); return NIL(); }
Value v_rfill(Value vx,Value vy,Value vw,Value vh,Value vc,Value vr){ int x=vx.n,y=vy.n,w=vw.n,h=vh.n,r=vr.n; u32 c=(u32)vc.n; for(int j=0;j<h;j++)for(int i=0;i<w;i++)if(in_round(i,j,w,h,r))putpx(x+i,y+j,c); return NIL(); }
Value v_rgrad(Value vx,Value vy,Value vw,Value vh,Value vt,Value vb,Value vr){ int x=vx.n,y=vy.n,w=vw.n,h=vh.n,r=vr.n; u32 ct=(u32)vt.n,cb=(u32)vb.n; for(int j=0;j<h;j++){ u32 c=lerpc(ct,cb,j,h-1); for(int i=0;i<w;i++)if(in_round(i,j,w,h,r))putpx(x+i,y+j,c);} return NIL(); }
Value v_rblend(Value vx,Value vy,Value vw,Value vh,Value vc,Value va,Value vr){ int x=vx.n,y=vy.n,w=vw.n,h=vh.n,r=vr.n,a=va.n; u32 c=(u32)vc.n; for(int j=0;j<h;j++)for(int i=0;i<w;i++)if(in_round(i,j,w,h,r))blendpx_at(x+i,y+j,c,a); return NIL(); }
Value v_circle(Value vx,Value vy,Value vr,Value vc){ int x=vx.n,y=vy.n,r=vr.n; u32 c=(u32)vc.n; for(int j=-r;j<=r;j++)for(int i=-r;i<=r;i++)if(i*i+j*j<=r*r)putpx(x+i,y+j,c); return NIL(); }
Value v_text_at(Value x,Value y,Value s,Value c){ drawtext(x.n,y.n,tostr(s),(u32)c.n,1); return NIL(); }
Value v_text_big(Value x,Value y,Value s,Value c){ drawtext(x.n,y.n,tostr(s),(u32)c.n,2); return NIL(); }
Value v_text_huge(Value x,Value y,Value s,Value c){ drawtext(x.n,y.n,tostr(s),(u32)c.n,4); return NIL(); }
Value v_background(Value c){ u32 col=(u32)c.n; for(u32 i=0;i<SW*SH;i++){WALL[i]=col;BACK[i]=col;} return NIL(); }
Value v_clear(void){ for(u32 i=0;i<SW*SH;i++)BACK[i]=WALL[i]; return NIL(); }
Value v_present(void){ SDL_UpdateTexture(g_tex,0,BACK,(int)(SW*4)); SDL_RenderClear(g_ren); SDL_RenderCopy(g_ren,g_tex,0,0); SDL_RenderPresent(g_ren); return NIL(); }
/* ---- input edges, randomness, more primitives ---- */
static SDL_Scancode keyscan(const char* n){
  if(!scmp(n,"left"))return SDL_SCANCODE_LEFT; if(!scmp(n,"right"))return SDL_SCANCODE_RIGHT;
  if(!scmp(n,"up"))return SDL_SCANCODE_UP; if(!scmp(n,"down"))return SDL_SCANCODE_DOWN;
  if(!scmp(n,"space"))return SDL_SCANCODE_SPACE; if(!scmp(n,"escape"))return SDL_SCANCODE_ESCAPE;
  if(!scmp(n,"return")||!scmp(n,"enter"))return SDL_SCANCODE_RETURN;
  if(slen(n)==1){char c=n[0]; if(c>='a'&&c<='z')return (SDL_Scancode)(SDL_SCANCODE_A+(c-'a'));}
  return SDL_SCANCODE_UNKNOWN;
}
Value v_pressed(Value name){ SDL_Scancode sc=keyscan(tostr(name)); if(sc==SDL_SCANCODE_UNKNOWN)return NUM(0); const Uint8* st=SDL_GetKeyboardState(0); return NUM((st[sc]&&!g_prev[sc])?1:0); }
static unsigned long g_seed=88172645463325252UL;
static unsigned long xrnd(void){ g_seed^=g_seed<<13; g_seed^=g_seed>>7; g_seed^=g_seed<<17; return g_seed; }
Value v_random(Value n){ long m=n.n>0?n.n:1; return NUM((long)(xrnd()%(unsigned long)m)); }
Value v_random_range(Value a,Value b){ long lo=a.n,hi=b.n; if(hi<lo){long t=lo;lo=hi;hi=t;} return NUM(lo+(long)(xrnd()%(unsigned long)(hi-lo+1))); }
Value v_line(Value vx0,Value vy0,Value vx1,Value vy1,Value vc){ int x0=vx0.n,y0=vy0.n,x1=vx1.n,y1=vy1.n; u32 c=(u32)vc.n; int dx=x1-x0,dy=y1-y0; int sx=dx<0?-1:1,sy=dy<0?-1:1; dx=dx<0?-dx:dx; dy=dy<0?-dy:dy; int err=(dx>dy?dx:-dy)/2,e2; for(;;){ putpx(x0,y0,c); if(x0==x1&&y0==y1)break; e2=err; if(e2>-dx){err-=dy;x0+=sx;} if(e2<dy){err+=dx;y0+=sy;} } return NIL(); }
Value v_rect(Value vx,Value vy,Value vw,Value vh,Value vc){ int x=vx.n,y=vy.n,w=vw.n,h=vh.n; u32 c=(u32)vc.n; for(int i=0;i<w;i++){putpx(x+i,y,c);putpx(x+i,y+h-1,c);} for(int j=0;j<h;j++){putpx(x,y+j,c);putpx(x+w-1,y+j,c);} return NIL(); }
static int pal_lookup(Value pal,char ch,u32* out){ if(pal.t!=TM)return 0; for(long i=0;i<pal.m->len;i++){ if(pal.m->keys[i][0]==ch&&pal.m->keys[i][1]==0){ *out=(u32)pal.m->vals[i].n; return 1; } } return 0; }
/* sprite(x,y, rows, scale, palette): rows = list of strings, each char -> a colour in the palette map; ' ' and '.' are transparent */
Value v_sprite(Value vx,Value vy,Value rows,Value vscale,Value pal){ int x=vx.n,y=vy.n,sc=vscale.n; if(sc<1)sc=1; if(rows.t!=TL)return NIL();
  for(long j=0;j<rows.l->len;j++){ Value rv=rows.l->items[j]; if(rv.t!=TS)continue; const char* s=rv.s; for(int i=0;s[i];i++){ char ch=s[i]; if(ch==' '||ch=='.')continue; u32 col; if(pal_lookup(pal,ch,&col)) fillrect(x+i*sc,y+j*sc,sc,sc,col); } }
  return NIL(); }
/* window(w,h): set the window size (call once at the start of your game) */
Value v_window(Value vw,Value vh){ int w=vw.n,h=vh.n; if(w<120)w=120; if(h<120)h=120; SW=w; SH=h;
  SDL_SetWindowSize(g_win,w,h); SDL_SetWindowPosition(g_win,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
  if(g_tex)SDL_DestroyTexture(g_tex); g_tex=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,SW,SH); SDL_SetTextureBlendMode(g_tex,SDL_BLENDMODE_NONE);
  BACK=(u32*)galloc((long)SW*SH*4); WALL=(u32*)galloc((long)SW*SH*4); for(u32 i=0;i<SW*SH;i++){BACK[i]=0;WALL[i]=0;} return NIL(); }
/* ---- sound: a tiny square-wave synth running on the SDL audio thread ---- */
#define MAXTONE 8
static struct { int freq,left,phase; } g_tones[MAXTONE];
static SDL_AudioDeviceID g_audio;
static void audio_cb(void* u,Uint8* stream,int len){ (void)u; Sint16* out=(Sint16*)stream; int n=len/2;
  for(int i=0;i<n;i++){ int s=0,act=0; for(int t=0;t<MAXTONE;t++){ if(g_tones[t].left>0){ int f=g_tones[t].freq?g_tones[t].freq:440; int period=44100/f,half=period/2; s+=(g_tones[t].phase<half)?2400:-2400; if(++g_tones[t].phase>=period)g_tones[t].phase=0; g_tones[t].left--; act++; } } if(act>1)s/=act; out[i]=(Sint16)s; } }
Value v_sound(Value f,Value ms){ int freq=(int)f.n,dur=(int)ms.n; if(g_audio){ SDL_LockAudioDevice(g_audio); for(int t=0;t<MAXTONE;t++)if(g_tones[t].left<=0){ g_tones[t].freq=freq; g_tones[t].phase=0; g_tones[t].left=dur*44100/1000; break; } SDL_UnlockAudioDevice(g_audio); } return NIL(); }
Value v_gc_mark(void){ return NIL(); }      /* kept for kernel API compatibility */
Value v_frame_reset(void){ return NIL(); }  /* no-op now: malloc heap, state persists */
static Value SAY(Value v){ printf("%s\n", tostr(v)); fflush(stdout); return NIL(); }

extern void kmain(void);
int main(int argc, char** argv){
  g_stack_bottom=(void*)&argc;   /* the bottom of the C stack, for the GC's root scan */
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)!=0){ fprintf(stderr,"SDL init failed: %s\n",SDL_GetError()); return 1; }
  g_win=SDL_CreateWindow("Vanta Game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW, SH, SDL_WINDOW_SHOWN);
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  g_tex=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,SW,SH);
  SDL_SetTextureBlendMode(g_tex,SDL_BLENDMODE_NONE);
  g_seed ^= SDL_GetPerformanceCounter();
  SDL_AudioSpec want; memset(&want,0,sizeof(want)); want.freq=44100; want.format=AUDIO_S16SYS; want.channels=1; want.samples=512; want.callback=audio_cb;
  g_audio=SDL_OpenAudioDevice(0,0,&want,0,0); if(g_audio) SDL_PauseAudioDevice(g_audio,0);
  BACK=(u32*)galloc((long)SW*SH*4); WALL=(u32*)galloc((long)SW*SH*4);
  for(u32 i=0;i<SW*SH;i++){BACK[i]=0;WALL[i]=0;}
  mx=SW/2; my=SH/2;
  kmain();
  SDL_DestroyTexture(g_tex); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
  return 0;
}
