/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
**************************************************************************/
/*
** This module contains C code that generates VDBE code used to process
** the WHERE clause of SQL statements.  This module is responsible for
** generating the code that loops through a table looking for applicable
** rows.  Indices are selected and used to speed the search when doing
** so is applicable.  Because this module is responsible for selecting
** indices, you might also think of this module as the "query optimizer".
**
** ���ģ�����C���Դ��룬�ô�����������ִ��SQL����WHERE�Ӿ��VDBE���롣���ģ�鸺�����ɴ��룬������ɨ��һ���������Һ��ʵ��С�
** ����������ʱ��ѡ��ʹ���������ӿ��ѯ����Ϊ���ģ�鸺������ѡ����Ҳ���԰����ģ�鵱��"��ѯ�Ż�"��
*/
#include "sqliteInt.h"


/*
** Trace output macros
** ���������,���ڲ��Ժ͵���
*/
#if defined(SQLITE_TEST) || defined(SQLITE_DEBUG)
int sqlite3WhereTrace = 0;
#endif
#if defined(SQLITE_TEST) && defined(SQLITE_DEBUG)
# define WHERETRACE(X)  if(sqlite3WhereTrace) sqlite3DebugPrintf X
#else
# define WHERETRACE(X)
#endif


/* Forward reference
** ǰ������ 
** ����ṹ��������
*/
typedef struct WhereClause WhereClause;
typedef struct WhereMaskSet WhereMaskSet;
typedef struct WhereOrInfo WhereOrInfo;
typedef struct WhereAndInfo WhereAndInfo;
typedef struct WhereCost WhereCost;

/*  ****************** WhereTerm����˵�� *********************
**
** The query generator uses an array of instances of this structure to
** help it analyze the subexpressions(�����) of the WHERE clause.  Each WHERE
** clause subexpression is separated from the others by AND operators,
** usually, or sometimes subexpressions separated by OR.
**
** ��ѯ������ʹ��һ��������ݽṹ��ʵ��������������WHERE�Ӿ���ӱ��ʽ��
** ÿ��WHERE�Ӿ��ӱ��ʽͨ���Ǹ���AND������ָ��ģ���ʱҲ�����OR�ָ���
**
** All WhereTerms are collected into a single WhereClause structure.  
** The following identity holds:
** 
** ����WhereTerms���ܵ�һ����һ��WhereClause�ṹ����.  
** ʹ�����·�ʽ����: 
**
**        WhereTerm.pWC->a[WhereTerm.idx] == WhereTerm
**
** ���term������ʽ:
**
**              X <op> <expr>
**
** where X is a column name and <op> is one of certain operators,
** then WhereTerm.leftCursor(���α�) and WhereTerm.u.leftColumn record the
** cursor number and column number for X.  WhereTerm.eOperator records
** the <op> using a bitmask(λ����) encoding defined by WO_xxx below.  The
** use of a bitmask encoding for the operator allows us to search
** quickly for terms that match any of several different operators.
**
** X��һ��������<op>��ĳ���������WhereTerm.leftCursorand��WhereTerm.u.leftColumn��¼X���α�����������
** WhereTerm.eOperatorʹ���������WO_xxx�����λ�����¼<op>��
** �����ʹ��λ����ʹ�������ܿ��ٲ����ܹ�ƥ�����ⲻͬ�������terms��
**
** A WhereTerm might also be two or more subterms connected by OR:
**
**         (t1.X <op> <expr>) OR (t1.Y <op> <expr>) OR ....
**
** In this second case, wtFlag as the TERM_ORINFO set and eOperator==WO_OR
** and the WhereTerm.u.pOrInfo field points to auxiliary information(������Ϣ) that
** is collected about the OR clause.
**
** һ��WhereTerm�ṹҲ��������OR���ӵ�2������subterms
**         (t1.X <op> <expr>) OR (t1.Y <op> <expr>) OR ....
** ����������£�wtFlag��ΪTERM_ORINFO��eOperator==WO_OR����WhereTerm.u.pOrInfoָ���ռ��Ĺ���OR�Ӿ�ĸ�����Ϣ��
**
** If a term in the WHERE clause does not match either of the two previous
** categories(ǰ����), then eOperator==0.  The WhereTerm.pExpr field is still set
** to the original subexpression content and wtFlags is set up(����) appropriately(�ʵ���)
** but no other fields in the WhereTerm object are meaningful.
**
** �����WHERE�Ӿ��е�һ��term��ǰ�����඼��ƥ�䣬��ôeOperator==0��WhereTerm.pExpr�Ծ���Ϊԭʼ�ӱ��ʽ�����ݣ�
** ����wtFlags����Ϊ�ʵ���ֵ������,whereTerm�����е������ֶζ���������ġ�
**
** When eOperator!=0, prereqRight and prereqAll record sets of cursor numbers,
** but they do so indirectly(��ӵ�).  
** A single WhereMaskSet structure translates cursor number 
** into bits and the translated bit is stored in the prereq fields(���ֶ�).  
** The translation is used in order to(Ϊ��) maximize the number of
** bits that will fit in(��Ӧ��װ���) a Bitmask(λ����).  
** The VDBE cursor numbers might be spread out(��Ϊ) over the non-negative(�Ǹ���) integers.  
** For example, the cursor numbers might be 3, 8, 9, 10, 20, 23, 41, and 45.  
** The WhereMaskSet translates these sparse(ϡ��ģ�ϡ�ٵ�) cursor numbers into consecutive integers(��������)
** beginning with 0 in order to make the best possible use of the available bits in the Bitmask.  
** So, in the example above, the cursor numbers would be mapped into integers 0 through 7.
** 
** ��eOperator != 0,prereqRight��prereqAll��ӵؼ�¼�α�������
** һ��������WhereMaskSet�ṹ����α���ת��Ϊbits����ת�����bit�洢��prereq�ֶ��С�
** ʹ��ת����Ϊ�����bitsʹ֮��Ӧһ��λ���롣VDBE�α�����ɢΪ�Ǹ�������
** ���磬�α���������3,8,9,10,20,23,41,45.
** WhereMaskSet����Щ��ɢ���α���ת��Ϊ��0��ʼ��������������Ϊ�˾�������������λ�����еĿ���λ��
** ���ԣ�������������У��α�����ӳ��Ϊ��0-7��������
**
** The number of terms(����) in a join is limited by(������) the number of bits
** in prereqRight and prereqAll.  The default is 64 bits, hence(���) SQLite
** is only able to process(����) joins with 64 or fewer tables.
** 
** һ�������е�terms��Ŀ��prereqRight and prereqAll�е�bits�����ơ�
** Ĭ����64λ�����SQLiteֻ�ܴ���64������ٸ�������ӡ� 
*/

/* WhereTerm �ṹ�������洢 where �Ӿ���� <op>(AND��OR)�ָ���ĸ����Ӿ� */
/*Expr �ṹ�壬��ʾ �﷨�������е�һ�����ʽ��ÿ���ڵ��Ǹýṹ��һ��ʵ���� ����sqliteInit.h 1829��*/
typedef struct WhereTerm WhereTerm;
struct WhereTerm {
  Expr *pExpr;            /* Pointer to the subexpression(�ӱ��ʽ) that is this term ָ������ӱ��ʽ��ָ�� */
  int iParent;            /* Disable pWC->a[iParent] when this term disabled �����term����ʱ����pWC->a[iParent] */
  int leftCursor;         /* Cursor number of X in "X <op> <expr>"  ��"X <op> <expr>"�е�X���α��� */
  union {
    int leftColumn;         /* Column number of X in "X <op> <expr>"  ��"X <op> <expr>"�е�X������ */
    WhereOrInfo *pOrInfo;   /* Extra information if eOperator==WO_OR ���eOperator==WO_ORʱ�Ķ�����Ϣ */
    WhereAndInfo *pAndInfo; /* Extra information if eOperator==WO_AND ���eOperator==WO_ANDʱ�Ķ�����Ϣ */
  } u;
  u16 eOperator;          /* A WO_xx value describing <op>  ����<op>��һ�� WO_xx */
  u8 wtFlags;             /* TERM_xxx bit flags.  See below   TERM_xxx bit��־�������TERM_xxx�����˾���ֵ */
  u8 nChild;              /* Number of children that must disable us ������õĺ����� */
  WhereClause *pWC;       /* The clause this term is part of  ���term���ĸ��Ӿ��һ����  */
  Bitmask prereqRight;    /* Bitmask of tables used by pExpr->pRight  pExpr->pRightʹ�õı�λ���� */
  Bitmask prereqAll;      /* Bitmask of tables referenced by pExpr  ��pExpr���õı�λ���� */
};
/*
** Allowed values of WhereTerm.wtFlags
** wtFlags�Ŀ���ֵ
*/
#define TERM_DYNAMIC    0x01   /* Need to call sqlite3ExprDelete(db, pExpr)  ��Ҫ����sqlite3ExprDelete(db, pExpr) */
#define TERM_VIRTUAL    0x02   /* Added by the optimizer.  Do not code  ���Ż�����ӣ�����Ҫ����*/
#define TERM_CODED      0x04   /* This term(��) is already coded  ���term�Ѿ��������� */
#define TERM_COPIED     0x08   /* Has a child  ��һ����term */
#define TERM_ORINFO     0x10   /* Need to free the WhereTerm.u.pOrInfo object  ��Ҫ�ͷ�WhereTerm.u.pOrInfo���� */
#define TERM_ANDINFO    0x20   /* Need to free the WhereTerm.u.pAndInfo obj  ��Ҫ�ͷ�WhereTerm.u.pAndInfo���� */
#define TERM_OR_OK      0x40   /* Used during OR-clause processing  ��OR�Ӿ�ִ��ʱʹ�� */

#ifdef SQLITE_ENABLE_STAT3
#  define TERM_VNULL    0x80   /* Manufactured x>NULL or x<=NULL term  ���� x>NULL or x<=NULL ��term */
#else
#  define TERM_VNULL    0x00   /* Disabled if not using stat3  �����ʹ��stat3����� */
#endif

/* ****************** WhereClause����˵�� *********************
** Explanation of pOuter:  For a WHERE clause of the form
**
**           a AND ((b AND c) OR (d AND e)) AND f
**
** There are separate WhereClause objects for the whole clause and for
** the subclauses "(b AND c)" and "(d AND e)".
** The pOuter field of the subclauses points to the WhereClause object for the whole clause.
**
** ����һ��pOuter: ����������ʽ��WHERE�Ӿ�:
** 			a AND ((b AND c) OR (d AND e)) AND f
** 
** ��������where�Ӿ�ͷ��Ӿ� "(b AND c)" and "(d AND e)"�����ڷֿ���WhereClause����
** �Ӿ��pOuter�ֶ�ָ������where�Ӿ��WhereClause����
*/

/*WhereClause�ṹ�����ڴ洢sql����е�������where�Ӿ䣬���ܰ���һ������WhereTerm��*/
struct WhereClause {
  Parse *pParse;           /* The parser context  ������������ */
  WhereMaskSet *pMaskSet;  /* Mapping of table cursor numbers to bitmasks  ����α�����λ����֮���ӳ�� */
  Bitmask vmask;           /* Bitmask identifying(ʶ��) virtual table cursors  ʶ������α��λ���� */
  WhereClause *pOuter;     /* Outer conjunction  ������ */
  u8 op;                   /* Split operator. TK_AND or TK_OR  �ָ�����TK_AND or TK_OR  */
  u16 wctrlFlags;          /* Might include WHERE_AND_ONLY  ���ܰ���WHERE_AND_ONLY */
  int nTerm;               /* Number of terms  terms����Ŀ */
  int nSlot;               /* Number of entries(��Ŀ��) in a[](153��)  ��һ��WhereTerm�еļ�¼��  */
  WhereTerm *a;            /* Each a[] describes(����) a term of the WHERE cluase  ÿ��a[]����WHERE�Ӿ��е�һ��term */
  
#if defined(SQLITE_SMALL_STACK)
  WhereTerm aStatic[1];    /* Initial static space for a[] ��ʼ��a[]��̬�ռ� */
#else
  WhereTerm aStatic[8];    /* Initial static space for a[] ��ʼ��a[]��̬�ռ� */
#endif
};

/* ****************** WhereOrInfo ����˵�� *********************
** A WhereTerm with eOperator==WO_OR has its u.pOrInfo pointer set to
** a dynamically allocated instance of the following structure.
**
** WhereTerm��eOperator�ֶ�ΪWO_OR�����WhereTerm��u.pOrInfoָ������Ϊ
** ���нṹ���һ����̬�����ʵ����
*/
struct WhereOrInfo {
  WhereClause wc;          /* Decomposition(�ֽ�) into subterms  �ֽ�Ϊ��terms */
  Bitmask indexable;       /* Bitmask of all indexable tables in the clause  where�Ӿ��е����пɼ������ı��λ���� */
};

/* ****************** WhereAndInfo ����˵�� *********************
** A WhereTerm with eOperator==WO_AND has its u.pAndInfo pointer set to
** a dynamically allocated instance of the following structure.
**
** ��WhereTerm��eOperator�ֶ�ΪWO_AND�����WhereTerm��u.pAndInfoָ������Ϊ
** ���нṹ���һ����̬�����ʵ����
*/
struct WhereAndInfo {
  WhereClause wc;          /* The subexpression broken out �ֽ���ӱ��ʽ*/
};

/* ****************** WhereMaskSet ����˵�� *********************
**
** An instance of the following structure keeps track of a mapping
** between VDBE cursor numbers and bits of the bitmasks in WhereTerm.
**
** �������ݽṹ��ʵ����¼VDBE�α�����WhereTerm�е�λ����bits֮���ӳ��
**
** SrcList_item.iCursor and Expr.iTable fields.  For any given WHERE 
** clause, the cursor numbers might not begin with 0 and they might
** contain gaps in the numbering sequence.  But we want to make maximum
** use of the bits in our bitmasks.  This structure provides a mapping
** from the sparse(ϡ�ٵ�) cursor numbers into consecutive integers(��������) beginning
** with 0.
**
** ������SrcList_item.iCursor �� Expr.iTable�ֶε�VDBE�α�����small���͡�
** �����κθ�����WHERE�Ӿ䣬�α������ܲ��Ǵ�0��ʼ�ģ����ҿ����ڱ���������пհס�
** ������ϣ����󻯵�ʹ��λ�����е�bits��������ݽṹ�ṩ��һ���ɷ�ɢ���α�������0��ʼ����������֮���ӳ��
**
** If WhereMaskSet.ix[A]==B it means that The A-th bit of a Bitmask
** corresponds(����) VDBE cursor number B.  The A-th bit of a bitmask is 1<<A.
**
** ���WhereMaskSet.ix[A]==B������ζ��λ�����A-th bit��VDBE�α���B���Ӧ��
** λ����A-th bit��1<<A
**
** For example, if the WHERE clause expression used these VDBE
** cursors:  4, 5, 8, 29, 57, 73.  Then the  WhereMaskSet structure
** would map those cursor numbers into bits 0 through 5.
**
** ���磬���WHERE�Ӿ���ʽʹ����ЩVDBE�α�: 4, 5, 8, 29, 57, 73.
** ��ôWhereMaskSet�ṹ�����Щ�α���ӳ��Ϊ0��5��
**
** Note that the mapping is not necessarily ordered.  In the example
** above, the mapping might go like this:  4->3, 5->1, 8->2, 29->0,
** 57->5, 73->4.  Or one of 719 other combinations(���) might be used. It
** does not really matter.  What is important is that sparse cursor
** numbers all get mapped into bit numbers that begin with 0 and contain
** no gaps.
**
** ע��:ӳ�䲻һ�������������������ӳ�������������:4->3, 5->1, 8->2, 29->0, 57->5, 73->4.
** ����������719������е�һ�֡�ʵ���ϣ��Ⲣ����Ҫ��
** ��Ҫ���Ƿ�ɢ���α�����ȫ��ӳ�䵽��0��ʼ��bit�������Ҳ��ܰ����հ�.
*/
struct WhereMaskSet {
  int n;                        /* Number of assigned(�ѷ����) cursor values  �ѷ����α�ֵ����Ŀ */
  int ix[BMS];                  /* Cursor assigned to each bit  �α걻�����ÿһ��bit */
};

/*
** A WhereCost object records a lookup strategy and the estimated
** cost of pursuing that strategy.
** WhereCost �����¼һ����ѯ�����Լ�ִ�иò��Ե�Ԥ���ɱ���
*/
struct WhereCost {
  WherePlan plan;    /* The lookup strategy ��ѯ���� */
  double rCost;      /* Overall cost(�ܳɱ�) of pursuing this search strategy ִ�иò�ѯ���Ե��ܳɱ� */
  Bitmask used;      /* Bitmask of cursors used by this plan �������ʹ�õ��α��λ���� */
};

/*
** Bitmasks for the operators that indices(index������Ŀ¼) are able to exploit(����).  
** An OR-ed combination of(...�����) these values can be used when searching for
** terms in the where clause.
**
** �������λ���룬�������Խ��п�������Щֵ��OR-ed��Ͽ����ڲ���where�Ӿ��termsʱ��ʹ��.
*/
#define WO_IN     0x001	//16����0000 0000 0001
#define WO_EQ     0x002	//16����0000 0000 0010
#define WO_LT     (WO_EQ<<(TK_LT-TK_EQ))	//(WO_EQ ���� TK_LT-TK_EQ)
#define WO_LE     (WO_EQ<<(TK_LE-TK_EQ))	//(WO_EQ ���� TK_LE-TK_EQ)
#define WO_GT     (WO_EQ<<(TK_GT-TK_EQ))	//(WO_EQ ���� TK_GT-TK_EQ)
#define WO_GE     (WO_EQ<<(TK_GE-TK_EQ))	//(WO_EQ ���� TK_GE-TK_EQ)
#define WO_MATCH  0x040	//16����0000 0100 0000
#define WO_ISNULL 0x080	//16����0000 1000 0000
#define WO_OR     0x100 //16����0001 0000 0000  /* Two or more OR-connected terms  ����������OR-connected terms */
#define WO_AND    0x200 //16����0010 0000 0000  /* Two or more AND-connected terms  ����������AND-connected terms */
#define WO_NOOP   0x800 //16����1000 0000 0000  /* This term does not restrict(����) search space ���term�����������ռ� */
#define WO_ALL    0xfff //16����1111 1111 1111  /* Mask of all possible WO_* values  ���п��ܵ�WO_*ֵ������ */
#define WO_SINGLE 0x0ff //16����0000 1111 1111  /* Mask of all non-compound(����ϵ�) WO_* values  ���в���ϵ�WO_*ֵ������ */

/*
** Value for wsFlags returned by bestIndex() and stored in WhereLevel.wsFlags.  
** These flags determine which search strategies are appropriate.
** 
** wsFlags��ֵ��bestIndex()���أ����Ҵ洢��WhereLevel.wsFlags�С�
** ��ЩwsFlagsֵ������Щ��ѯ�����Ǻ��ʵġ�
**
** The least significant(�����Ч��) 12 bits is reserved as(��������) a mask for WO_ values above(����).
** The WhereLevel.wsFlags field is usually set to WO_IN|WO_EQ|WO_ISNULL.
** But if the table is the right table of a left join, WhereLevel.wsFlags
** is set to WO_IN|WO_EQ. 
**
** ��λ��12λ���������е�WO_ ֵ�����롣
** WhereLevel.wsFlags�ֶ�ͨ������ΪWO_IN|WO_EQ|WO_ISNULL.
** ������������ʾ�����ӵ��ұ�WhereLevel.wsFlags����ΪWO_IN|WO_EQ.
**
** The WhereLevel.wsFlags field can then be used as
** the "op" parameter(����) to findTerm when we are resolving(����) equality constraints(��ʽԼ��).
** ISNULL constraints(Լ��) will then not be used on the right table of a left join.
**
** �����Ƿ�����ʽԼ��ʱ��WhereLevel.wsFlags�ֶε���findTerm��"op"����ʹ��
** �������ӵ��ұ��ϲ���ʹ��ISNULLԼ��
** Tickets #2177 and #2189.
*/
#define WHERE_ROWID_EQ     0x00001000  /* rowid=EXPR or rowid IN (...)  rowid=EXPR��rowid IN(...) */
#define WHERE_ROWID_RANGE  0x00002000  /* rowid<EXPR and/or rowid>EXPR  rowid<EXPR ��/�� rowid>EXPR */
#define WHERE_COLUMN_EQ    0x00010000  /* x=EXPR or x IN (...) or x IS NULL  x=EXPR �� x IN (...) �� x IS NULL  */
#define WHERE_COLUMN_RANGE 0x00020000  /* x<EXPR and/or x>EXPR */
#define WHERE_COLUMN_IN    0x00040000  /* x IN (...) */
#define WHERE_COLUMN_NULL  0x00080000  /* x IS NULL */
#define WHERE_INDEXED      0x000f0000  /* Anything that uses an index  �κ�ʹ������ */
#define WHERE_NOT_FULLSCAN 0x100f3000  /* Does not do a full table scan  ����Ҫȫ��ɨ�� */
#define WHERE_IN_ABLE      0x000f1000  /* Able to support an IN operator  ֧��IN���� */
#define WHERE_TOP_LIMIT    0x00100000  /* x<EXPR or x<=EXPR constraint  x<EXPR or x<=EXPRԼ�� */
#define WHERE_BTM_LIMIT    0x00200000  /* x>EXPR or x>=EXPR constraint  x>EXPR or x>=EXPRԼ�� */
#define WHERE_BOTH_LIMIT   0x00300000  /* Both x>EXPR and x<EXPR */
#define WHERE_IDX_ONLY     0x00800000  /* Use index only - omit table  ֻ��������ʡ�Ա� */
#define WHERE_ORDERBY      0x01000000  /* Output will appear in correct order  ��ǡ����˳����� */
#define WHERE_REVERSE      0x02000000  /* Scan in reverse order  ����ɨ�� */
#define WHERE_UNIQUE       0x04000000  /* Selects no more than(ֻ�ǣ�����) one row  ֻѡ��һ�� */
#define WHERE_VIRTUALTABLE 0x08000000  /* Use virtual-table processing(����)  ʹ��������� */
#define WHERE_MULTI_OR     0x10000000  /* OR using multiple indices  ORʹ�ö������� */
#define WHERE_TEMP_INDEX   0x20000000  /* Uses an ephemeral index  ʹ����ʱ���� */
#define WHERE_DISTINCT     0x40000000  /* Correct order for DISTINCT  DISTINCT����ȷ˳�� */

/*
** Initialize a preallocated(Ԥ����) WhereClause structure.
** ��ʼ��һ��Ԥ�����WhereClause���ݽṹ
*/
static void whereClauseInit(
  WhereClause *pWC,        /* The WhereClause to be initialized  ������ʼ��Where�Ӿ� */
  Parse *pParse,           /* The parsing context  ������������ */
  WhereMaskSet *pMaskSet,  /* Mapping from table cursor numbers to bitmasks  ����α�����λ�����ӳ�� */
  u16 wctrlFlags           /* Might include WHERE_AND_ONLY  ���ܰ���WHERE_AND_ONLY */
){
  pWC->pParse = pParse;	 /*��ʼ��������������*/
  pWC->pMaskSet = pMaskSet;	 /*��ʼ��pMaskSet*/
  pWC->pOuter = 0;	 /*������Ϊ��*/
  pWC->nTerm = 0;	/*where�Ӿ������Ϊ0*/
  pWC->nSlot = ArraySize(pWC->aStatic);	//��ʼ��a[]��̬�ռ��Ԫ�ظ�����ArraySize���������Ԫ�ظ�����
  pWC->a = pWC->aStatic;	/*��ʼ��where�Ӿ��е�WhereTerm���ݽṹ*/
  pWC->vmask = 0;	/*��ʼ������α��λ����*/
  pWC->wctrlFlags = wctrlFlags;		/*��ʼ��wctrlFlags*/
}

/* Forward reference(ǰ������) ���庯�������� */
static void whereClauseClear(WhereClause*);

/*
** Deallocate all memory associated with a WhereAndInfo object.
** �ͷ���WhereAndInfo����������������ڴ�
*/
static void whereAndInfoDelete(sqlite3 *db, WhereAndInfo *p){
  whereClauseClear(&p->wc);	 /*�������*/
  sqlite3DbFree(db, p);	 /*�ͷű�������һ���ض����ݿ����ӵ��ڴ�*/
}

/*
** Deallocate a WhereClause structure.  The WhereClause structure
** itself is not freed.  This routine is the inverse of whereClauseInit().
** ���һ��WhereClause���ݽṹ�ķ��䡣WhereClause�������ͷš����������whereClauseInit()�ķ���ִ��
*/
static void whereClauseClear(WhereClause *pWC){
  int i;
  WhereTerm *a;
  sqlite3 *db = pWC->pParse->db;	/*���ݿ����ӵ�ʵ����Ϊwhere�Ӿ����ڵ����ݿ�*/
  for(i=pWC->nTerm-1, a=pWC->a; i>=0; i--, a++){	/*ѭ������where�Ӿ��ÿ��term��*/
    if( a->wtFlags & TERM_DYNAMIC ){	/*�����ǰ��term�Ƕ�̬��*/
      sqlite3ExprDelete(db, a->pExpr);	/*����sqlite3ExprDelete()�ݹ�ɾ��term�еı��ʽ��*/
    }
    if( a->wtFlags & TERM_ORINFO ){		/*�����ǰ��term�洢����OR�Ӿ����Ϣ*/
      whereOrInfoDelete(db, a->u.pOrInfo);	/*�����ǰterm�е�pOrInfo����������ڴ����*/
    }else if( a->wtFlags & TERM_ANDINFO ){	/*�����ǰ��term�洢����AND�Ӿ����Ϣ*/
      whereAndInfoDelete(db, a->u.pAndInfo);  /*�����ǰterm�е�pAndInfo����������ڴ����*/
    }
  }
  if( pWC->a!=pWC->aStatic ){	/*���where�Ӿ��term��ǳ�ʼ��̬�ռ�*/
    sqlite3DbFree(db, pWC->a);	/*�ͷű�������һ���ض����ݿ����ӵ��ڴ�*/
  }
}

/*
** ��ӵ����� WhereTerm ���� WhereClause ���� pWC�С�
**
** The new WhereTerm object is constructed from Expr p and with wtFlags.
** The index in pWC->a[] of the new WhereTerm is returned on success.
** 0 is returned if the new WhereTerm could not be added due to a memory
** allocation error.  The memory allocation failure will be recorded in
** the db->mallocFailed flag so that higher-level functions can detect it.
**
** �µ�WhereTerm�������Expr��wtFlags������
** ���ɹ���������WhereTerm��pWC->a[]��������
** ��������ڴ��������µ�WhereTerm������ӵ�WhereClause�У��򷵻�0.
** �ڴ����ʧ�ܻᱻ��¼��db->mallocFailed��־�У��Ա���߼��ĺ�����⵽����
**
** This routine will increase the size of the pWC->a[] array as necessary.
**
** �����Ҫ�Ļ������������pWC->a[]����Ĵ�С��
**
** If the wtFlags argument includes TERM_DYNAMIC, then responsibility
** for freeing the expression p is assumed by the WhereClause object pWC.
** This is true even if this routine fails to allocate a new WhereTerm.
**
** ���wtFlags����TERM_DYNAMIC����ôWhereClause�Ķ���pWC�Ḻ���ͷű��ʽp��
** �����������û�ܳɹ�����һ���µ�WhereTerm����Ҳ����ȷ�ġ�
**
** WARNING:  This routine might reallocate the space used to store
** WhereTerms.  All pointers to WhereTerms should be invalidated after
** calling this routine.  Such pointers may be reinitialized by referencing
** the pWC->a[] array.
**
** ����:���������ܻ����·������ڴ洢WhereTerms�Ŀռ䡣����ָ��WhereTerms��ָ���ڵ����������󶼻�ʧЧ��
** ��Щָ����ܻ�ͨ������pWC->a[]����������
*/
static int whereClauseInsert(WhereClause *pWC, Expr *p, u8 wtFlags){
  WhereTerm *pTerm;   /* �½�һ��WhereTerm */
  int idx;
  testcase( wtFlags & TERM_VIRTUAL );  /* EV: R-00211-15100 */
  if( pWC->nTerm>=pWC->nSlot ){	 /*���WhereClause�е�term�����ڻ������һ��WhereTerms�еļ�¼��*/
    WhereTerm *pOld = pWC->a;	/*���嵱ǰ��termΪpOld*/
    sqlite3 *db = pWC->pParse->db;	/*���嵱ǰwhere�Ӿ�����ݿ�����*/
    pWC->a = sqlite3DbMallocRaw(db, sizeof(pWC->a[0])*pWC->nSlot*2 );	/*�����ڴ�*/
    if( pWC->a==0 ){	/*��������ڴ�ʧ��*/
      if( wtFlags & TERM_DYNAMIC ){	 /*�����ǰterm�Ƕ�̬�����*/
        sqlite3ExprDelete(db, p);	/*�ݹ�ɾ��term�еı��ʽ��*/
      }
      pWC->a = pOld;	/*��term����Ϊ֮ǰ��¼��term*/
      return 0;	 /*����0*/
    }
    memcpy(pWC->a, pOld, sizeof(pWC->a[0])*pWC->nTerm); 
    if( pOld!=pWC->aStatic ){
      sqlite3DbFree(db, pOld);	/*�ͷſ��ܱ�������һ���ض����ݿ����ӵ��ڴ�*/
    }
    pWC->nSlot = sqlite3DbMallocSize(db, pWC->a)/sizeof(pWC->a[0]);
  }
  pTerm = &pWC->a[idx = pWC->nTerm++];
  pTerm->pExpr = p;
  pTerm->wtFlags = wtFlags;
  pTerm->pWC = pWC;
  pTerm->iParent = -1;
  return idx;
}

/*
** This routine identifies subexpressions in the WHERE clause where
** each subexpression is separated by the AND operator or some other
** operator specified in the op parameter.  The WhereClause structure
** is filled with pointers to subexpressions.  
**
** �������ʶ����WHERE�Ӿ��е��ӱ��ʽ����Щ�ӱ��ʽ����AND���������op�����ж��������������ָ���
** WhereClause���ݽṹ�洢ָ����ӱ��ʽ��ָ�롣
**
** For example:
**
**    WHERE  a=='hello' AND coalesce(b,11)<10 AND (c+12!=d OR c==22)
**           \________/     \_______________/     \________________/
**            slot[0]            slot[1]               slot[2]
** 
** The original WHERE clause in pExpr is unaltered.  All this routine
** does is make slot[] entries point to substructure within pExpr.
**
** ����:
**    WHERE  a=='hello' AND coalesce(b,11)<10 AND (c+12!=d OR c==22)
**           \________/     \_______________/     \________________/
**            slot[0]            slot[1]               slot[2]
**
** ��pExpr�е�ԭʼ��WHERE�Ӿ��ǲ���ġ�������������slot[]�ļ�¼ָ����pExpr�е��ӽṹ
**
** In the previous sentence and in the diagram, "slot[]" refers to
** the WhereClause.a[] array.  The slot[] array grows as needed to contain
** all terms of the WHERE clause.
**
** ��ǰ��ľ��Ӻ���ͼ���У�"slot[]"ָ����WhereClause.a[]���顣slot���������Ҫ����Ҫ����WHERE�Ӿ������terms
**
*/
static void whereSplit(WhereClause *pWC, Expr *pExpr, int op){
  pWC->op = (u8)op;  /*��ʼ��WHERE�Ӿ���зָ�������*/
  if( pExpr==0 ) return;
  if( pExpr->op!=op ){  /*������ʽ�Ĳ���������ָ���������*/
    whereClauseInsert(pWC, pExpr, 0); /*��pWC�Ӿ��в���һ��WhereTerm*/
  }else{
    whereSplit(pWC, pExpr->pLeft, op);	/*�ݹ�ָ�WHERE�Ӿ������ʽ*/
    whereSplit(pWC, pExpr->pRight, op);	 /*�ݹ�ָ�WHERE�Ӿ���ұ��ʽ*/
  }
}

/*
** Initialize an expression mask set (a WhereMaskSet object)
**
** ��ʼ��һ�����ʽ��������(һ��WhereMaskSet����)
*/
#define initMaskSet(P)  memset(P, 0, sizeof(*P))

/*
** Return the bitmask for the given cursor number.  Return 0 if
** iCursor is not in the set.
**
** ���ظ������α�����λ���롣���iCursorδ���ã��򷵻�0
*/
static Bitmask getMask(WhereMaskSet *pMaskSet, int iCursor){
  int i;
  assert( pMaskSet->n<=(int)sizeof(Bitmask)*8 );  /*�ж�*/
  for(i=0; i<pMaskSet->n; i++){
    if( pMaskSet->ix[i]==iCursor ){
      return ((Bitmask)1)<<i;
    }
  }
  return 0;
}

/*
** Create a new mask for cursor iCursor.
** Ϊ�α�iCursor����һ���µ�����
** 
** There is one cursor per table in the FROM clause.  The number of
** tables in the FROM clause is limited by a test early in the
** sqlite3WhereBegin() routine.  So we know that the pMaskSet->ix[]
** array will never overflow.
**
** ��FROM�Ӿ��е�ÿ������һ���αꡣ��FROM�Ӿ��еı�ĸ�����sqlite3WhereBegin()�����һ���������ơ�
** ��������֪��pMaskSet->ix[]��Զ���������
**
*/
static void createMask(WhereMaskSet *pMaskSet, int iCursor){
  assert( pMaskSet->n < ArraySize(pMaskSet->ix) );
  pMaskSet->ix[pMaskSet->n++] = iCursor;
}

/*
** This routine walks (recursively) an expression tree and generates
** a bitmask indicating which tables are used in that expression
** tree.
**
** �������(�ݹ��)����һ�����ʽ����������һ��λ����ָʾ�ñ��ʽ��ʹ������Щ��
**
** In order for this routine to work, the calling function must have
** previously invoked sqlite3ResolveExprNames() on the expression.  
** See the header comment on that routine for additional information.
** The sqlite3ResolveExprNames() routines looks for column names and
** sets their opcodes to TK_COLUMN and their Expr.iTable fields to
** the VDBE cursor number of the table.  This routine just has to
** translate the cursor numbers into bitmask values and OR all
** the bitmasks together.
**
** Ϊ������������������ú�������Ԥ�Ȼ��ѱ��ʽ�е�sqlite3ResolveExprNames()��
** �鿴sqlite3ResolveExprNames()����ͷ����ע����Ϣ��
** sqlite3ResolveExprNames()�����������������Щ�е�opcodes����ΪTK_COLUMN���������ǵ�Expr.iTable�ֶ���Ϊ���VDBE�α�����
** �������ֻ�ǰ��α���ת��Ϊλ���룬�����е�λ���뼯��������
*/
static Bitmask exprListTableUsage(WhereMaskSet*, ExprList*);
static Bitmask exprSelectTableUsage(WhereMaskSet*, Select*);

static Bitmask exprTableUsage(WhereMaskSet *pMaskSet, Expr *p){
  Bitmask mask = 0;
  if( p==0 ) return 0;
  if( p->op==TK_COLUMN ){
    mask = getMask(pMaskSet, p->iTable);
    return mask;
  }
  mask = exprTableUsage(pMaskSet, p->pRight);
  mask |= exprTableUsage(pMaskSet, p->pLeft);
  if( ExprHasProperty(p, EP_xIsSelect) ){
    mask |= exprSelectTableUsage(pMaskSet, p->x.pSelect);
  }else{
    mask |= exprListTableUsage(pMaskSet, p->x.pList);
  }
  return mask;
}

static Bitmask exprListTableUsage(WhereMaskSet *pMaskSet, ExprList *pList){
  int i;
  Bitmask mask = 0;
  if( pList ){
    for(i=0; i<pList->nExpr; i++){
      mask |= exprTableUsage(pMaskSet, pList->a[i].pExpr);
    }
  }
  return mask;
}

static Bitmask exprSelectTableUsage(WhereMaskSet *pMaskSet, Select *pS){
  Bitmask mask = 0;
  while( pS ){
    SrcList *pSrc = pS->pSrc;
    mask |= exprListTableUsage(pMaskSet, pS->pEList);
    mask |= exprListTableUsage(pMaskSet, pS->pGroupBy);
    mask |= exprListTableUsage(pMaskSet, pS->pOrderBy);
    mask |= exprTableUsage(pMaskSet, pS->pWhere);
    mask |= exprTableUsage(pMaskSet, pS->pHaving);
    if( ALWAYS(pSrc!=0) ){
      int i;
      for(i=0; i<pSrc->nSrc; i++){
        mask |= exprSelectTableUsage(pMaskSet, pSrc->a[i].pSelect);
        mask |= exprTableUsage(pMaskSet, pSrc->a[i].pOn);
      }
    }
    pS = pS->pPrior;
  }
  return mask;
}

/*
** Return TRUE if the given operator is one of the operators that is
** allowed for an indexable WHERE clause term.  The allowed operators are
** "=", "<", ">", "<=", ">=", and "IN".
**
** ����������������һ����������WHERE�Ӿ��term����ʹ�õ����������ô�ͷ���TRUE.
** ������������"=", "<", ">", "<=", ">=", and "IN"
**
** IMPLEMENTATION-OF: R-59926-26393 To be usable by an index a term must be
** of one of the following forms: column = expression column > expression
** column >= expression column < expression column <= expression
** expression = column expression > column expression >= column
** expression < column expression <= column column IN
** (expression-list) column IN (subquery) column IS NULL
**
** IMPLEMENTATION-OF: R-59926-26393 һ��term��ʹ�������������������֮һ:
** column = expression 
** column > expression
** column >= expression 
** column < expression 
** column <= expression
** expression = column 
** expression > column 
** expression >= column
** expression < column 
** expression <= column 
** column IN (expression-list) 
** column IN (subquery) 
** column IS NULL
*/
static int allowedOp(int op){
  assert( TK_GT>TK_EQ && TK_GT<TK_GE );
  assert( TK_LT>TK_EQ && TK_LT<TK_GE );
  assert( TK_LE>TK_EQ && TK_LE<TK_GE );
  assert( TK_GE==TK_EQ+4 );
  return op==TK_IN || (op>=TK_EQ && op<=TK_GE) || op==TK_ISNULL;
}

/*
** Swap two objects of type TYPE.
**
** ����TYPE���͵���������
*/
#define SWAP(TYPE,A,B) {TYPE t=A; A=B; B=t;}

/*
** Commute a comparison operator.  Expressions of the form "X op Y"
** are converted into "Y op X".
** ����һ���Ƚϲ�������"X op Y"���ʽת��Ϊ"Y op X"
**
** If a collation sequence is associated with either the left or right
** side of the comparison, it remains associated with the same side after
** the commutation. So "Y collate NOCASE op X" becomes 
** "X collate NOCASE op Y". This is because any collation sequence on
** the left hand side of a comparison overrides any collation sequence 
** attached to the right. For the same reason the EP_ExpCollate flag
** is not commuted.
**
** ���һ������������Ƚ�ʽ����߻��ұ���أ���������������ͬ���йء�
** ��� "Y collate NOCASE op X" ����� "X collate NOCASE op Y" ��
** ������Ϊ�κ��ڱȽ�ʽ��ߵ�����������д�ұߵ��κ��������С�
** Ҳ�ɴˣ�EP_ExpCollate��־��������
*/
static void exprCommute(Parse *pParse, Expr *pExpr){
  u16 expRight = (pExpr->pRight->flags & EP_ExpCollate);  /*pLeft��ʾ���ӽڵ㣬pRight��ʾ���ӽڵ㡣*/
  u16 expLeft = (pExpr->pLeft->flags & EP_ExpCollate);  /*EP_ExpCollate��ʾ��ȷ�涨����������*/
  assert( allowedOp(pExpr->op) && pExpr->op!=TK_IN );  /*�ж����ʽ���������"=", "<", ">", "<=", ">=", and "IN"��һ��*/
  pExpr->pRight->pColl = sqlite3ExprCollSeq(pParse, pExpr->pRight);  /*pColl��ʾ�е��������ͻ���Ϊ0*/
  pExpr->pLeft->pColl = sqlite3ExprCollSeq(pParse, pExpr->pLeft);  /*sqlite3ExprCollSeq()�������ر��ʽpExpr��Ĭ���������С����û��Ĭ���������ͣ�����0��*/
  SWAP(CollSeq*,pExpr->pRight->pColl,pExpr->pLeft->pColl);  /*�������������ڵ����������*/
  pExpr->pRight->flags = (pExpr->pRight->flags & ~EP_ExpCollate) | expLeft;  
  pExpr->pLeft->flags = (pExpr->pLeft->flags & ~EP_ExpCollate) | expRight;
  SWAP(Expr*,pExpr->pRight,pExpr->pLeft);  /*�������������ӽڵ�*/
  if( pExpr->op>=TK_GT ){
    assert( TK_LT==TK_GT+2 );
    assert( TK_GE==TK_LE+2 );
    assert( TK_GT>TK_EQ );
    assert( TK_GT<TK_LE );
    assert( pExpr->op>=TK_GT && pExpr->op<=TK_GE );
    pExpr->op = ((pExpr->op-TK_GT)^2)+TK_GT;
  }
}

/*
** Translate from TK_xx operator to WO_xx bitmask.
**
** ��TK_xx������ת��ΪWO_xxλ����
*/
static u16 operatorMask(int op){
  u16 c;
  assert( allowedOp(op) );
  if( op==TK_IN ){
    c = WO_IN;
  }else if( op==TK_ISNULL ){
    c = WO_ISNULL;
  }else{
    assert( (WO_EQ<<(op-TK_EQ)) < 0x7fff );
    c = (u16)(WO_EQ<<(op-TK_EQ));
  }
  assert( op!=TK_ISNULL || c==WO_ISNULL );
  assert( op!=TK_IN || c==WO_IN );
  assert( op!=TK_EQ || c==WO_EQ );
  assert( op!=TK_LT || c==WO_LT );
  assert( op!=TK_LE || c==WO_LE );
  assert( op!=TK_GT || c==WO_GT );
  assert( op!=TK_GE || c==WO_GE );
  return c;
}

/*
** Search for a term in the WHERE clause that is of the form "X <op> <expr>"
** where X is a reference to the iColumn of table iCur and <op> is one of
** the WO_xx operator codes specified by the op parameter.
** Return a pointer to the term.  Return 0 if not found.
**
** ��WHERE�Ӿ��в���һ��term�����term��"X <op> <expr>"��ʽ���ɡ�
** ����X�����iCur��iColumn��ص�,<op>����op����ָ����WO_xx�������һ�֡�
** ����һ��ָ��term��ָ�롣���û���ҵ��ͷ���0��
*/
static WhereTerm *findTerm(
  WhereClause *pWC,     /* The WHERE clause to be searched  Ҫ�����ҵ�WHERE�Ӿ� */
  int iCur,             /* Cursor number of LHS  LHS(��ʽ�����)���α��� */
  int iColumn,          /* Column number of LHS  LHS(��ʽ�����)������ */
  Bitmask notReady,     /* RHS must not overlap with this mask  ��ʽ���ұ߲����������ص� */
  u32 op,               /* Mask of WO_xx values describing operator  �����������WO_xxֵ������ */
  Index *pIdx           /* Must be compatible with this index, if not NULL  ������������һ�£�����Ϊ�� */
){
  WhereTerm *pTerm;  /* �½�һ��WhereTerm */
  int k;
  assert( iCur>=0 );
  op &= WO_ALL;
  for(; pWC; pWC=pWC->pOuter){
    for(pTerm=pWC->a, k=pWC->nTerm; k; k--, pTerm++){
      if( pTerm->leftCursor==iCur
         && (pTerm->prereqRight & notReady)==0
         && pTerm->u.leftColumn==iColumn
         && (pTerm->eOperator & op)!=0
      ){
        if( iColumn>=0 && pIdx && pTerm->eOperator!=WO_ISNULL ){
          Expr *pX = pTerm->pExpr;
          CollSeq *pColl;
          char idxaff;
          int j;
          Parse *pParse = pWC->pParse;
  
          idxaff = pIdx->pTable->aCol[iColumn].affinity;
          if( !sqlite3IndexAffinityOk(pX, idxaff) ) continue;
  
          /* Figure out the collation sequence required from an index for
          ** it to be useful for optimising expression pX. Store this
          ** value in variable pColl.
		  **
		  ** ��һ���������ҳ�������������У������Ż����ʽpX�� �����ֵ�洢�ڱ���pColl�С� 
          */
          assert(pX->pLeft);
		  /*sqlite3BinaryCompareCollSeq()����ָ���������е�ָ�룬������������һ�������ƱȽ������ʹ�ã����Ƚ�pLeft��pRight������expr.c 222��*/
          pColl = sqlite3BinaryCompareCollSeq(pParse, pX->pLeft, pX->pRight);
          assert(pColl || pParse->nErr);
  
          for(j=0; pIdx->aiColumn[j]!=iColumn; j++){
            if( NEVER(j>=pIdx->nColumn) ) return 0;
          }
          if( pColl && sqlite3StrICmp(pColl->zName, pIdx->azColl[j]) ) continue;
        }
        return pTerm;
      }
    }
  }
  return 0;
}

/* Forward reference ǰ������ */
static void exprAnalyze(SrcList*, WhereClause*, int);

/*
** Call exprAnalyze on all terms in a WHERE clause.  
**
** ��һ��WHERE�Ӿ������terms�ϵ���exprAnalyze
*/
static void exprAnalyzeAll(
  SrcList *pTabList,       /* the FROM clause  FROM�Ӿ� */
  WhereClause *pWC         /* the WHERE clause to be analyzed  ��������WHERE�Ӿ� */
){
  int i;
  for(i=pWC->nTerm-1; i>=0; i--){
    exprAnalyze(pTabList, pWC, i);
  }
}

#ifndef SQLITE_OMIT_LIKE_OPTIMIZATION
/*
** Check to see if the given expression is a LIKE or GLOB operator that
** can be optimized using inequality constraints.  Return TRUE if it is
** so and false if not.
**
** �鿴���ʽ�Ƿ��ǿ���ʹ�ò���ʽԼ�������Ż���LIKE��GLOB�������
** ��������򷵻�TRUE����������򷵻�FALSE.
**
** In order for the operator to be optimizible, the RHS must be a string
** literal that does not begin with a wildcard.  
**
** Ϊ���ܹ��Ż������������ʽ���ұ߱�����һ���ַ����Ҳ�����ͨ�����ͷ��
**
*/

/*Expr�ṹ�壬�﷨�������е�һ�����ʽ��ÿ���ڵ��Ǹýṹ��һ��ʵ���� ����sqliteInit.h 1829��*/
static int isLikeOrGlob(
  Parse *pParse,    /* Parsing and code generating context �����ͱ������������� */
  Expr *pExpr,      /* Test this expression  ������������ʽ */
  Expr **ppPrefix,  /* Pointer to TK_STRING expression with pattern prefix  ָ��ָ����ģʽǰ׺��TK_STRING���ʽ */
  int *pisComplete, /* True if the only wildcard is % in the last character  ���ֻ��һ��ͨ���"%"��������򷵻�True */
  int *pnoCase      /* True if uppercase is equivalent to lowercase  ������ִ�Сд�򷵻�True */
){
  const char *z = 0;         /* String on RHS of LIKE operator  LIKE������ұߵ��ַ��� */
  Expr *pRight, *pLeft;      /* Right and left size of LIKE operator  LIKE������ұߺ���ߵĴ�С */
  ExprList *pList;           /* List of operands to the LIKE operator  LIKE������Ĳ��������б� */
  int c;                     /* One character in z[]  z[]�����е�һ���ַ� */
  int cnt;                   /* Number of non-wildcard prefix characters  ��ͨ���ǰ׺���ַ�����Ŀ */
  char wc[3];                /* Wildcard characters  ͨ��� */
  sqlite3 *db = pParse->db;  /* Database connection  ���ݿ����� */
  sqlite3_value *pVal = 0;
  int op;                    /* Opcode of pRight  pRight��Opcode */

  if( !sqlite3IsLikeFunction(db, pExpr, pnoCase, wc) ){  /*�ж��Ƿ���LIKE����*/
    return 0;
  }
#ifdef SQLITE_EBCDIC
  if( *pnoCase ) return 0;
#endif
  pList = pExpr->x.pList;  
  pLeft = pList->a[1].pExpr;  
  if( pLeft->op!=TK_COLUMN 
	/*sqlite3ExprAffinity()�������ر��ʽpExpr���ڹ����� 'affinity'*/	  
   || sqlite3ExprAffinity(pLeft)!=SQLITE_AFF_TEXT   /*SQLITE_AFF_TEXT��ʾ�й������͡�*/
   || IsVirtual(pLeft->pTab)  /*���Ըñ��Ƿ���һ����� pTab��ʾ���ʽTK_COLUMN�ı�*/
  ){
    /* IMP: R-02065-49465 The left-hand side of the LIKE or GLOB operator must
    ** be the name of an indexed column with TEXT affinity. 
    **
	**IMP: R-02065-49465 LIKE��GLOB���������߱�����һ��TEXT�׺��ԵĴ�����������
	*/
    return 0;
  }
  assert( pLeft->iColumn!=(-1) );  /* Because IPK never has AFF_TEXT ��ΪIPK��û��TEXT�׺��� */

  pRight = pList->a[0].pExpr; 
  op = pRight->op;
  /*
  ** int iTable;            // TK_COLUMN: cursor number of table holding column	  TK_COLUMN:���г��й����
  **				           TK_REGISTER: register number			TK_REGISTER:�Ĵ�������
  **				           TK_TRIGGER: 1 -> new, 0 -> old 			TK_TRIGGER:1->��,0->��
  ** ynVar iColumn;         // TK_COLUMN: column index.  -1 for rowid.		TK_COLUMN:��������-1ΪROWID
  **				           TK_VARIABLE: variable number (always >= 1). 	TK_VARIABLE: ������(���Ǵ��ڵ���1)
  ** u8 op2;                // TK_REGISTER: original value of Expr.op		TK_REGISTER:Expr.op��ԭʼֵ
  **				           TK_COLUMN: the value of p5 for OP_Column		TK_COLUMN: ����OP_Column��P5��ֵ
  **				           TK_AGG_FUNCTION: nesting depth 			TK_AGG_FUNCTION: Ƕ�����
  ** AggInfo *pAggInfo;     // Used by TK_AGG_COLUMN and TK_AGG_FUNCTION 		TK_AGG_COLUMN��TK_AGG_FUNCTIONʹ��
  ** Table *pTab;           // Table for TK_COLUMN expressions. 			���ʽTK_COLUMN�ı�
  */
  if( op==TK_REGISTER ){
    op = pRight->op2;
  }
  if( op==TK_VARIABLE ){
    Vdbe *pReprepare = pParse->pReprepare;  /*pRepreparevia��ʾVM�����ޣ�sqlite3Reprepare()����*/
    int iCol = pRight->iColumn;
	/*sqlite3VdbeGetValue()��������һ��ָ��sqlite3_value�ṹ�壬����ṹ�������һ�������vʵ����iVar����ֵ��*/
    pVal = sqlite3VdbeGetValue(pReprepare, iCol, SQLITE_AFF_NONE);  /*SQLITE_AFF_NONE��ʾ�й�������*/
    if( pVal && sqlite3_value_type(pVal)==SQLITE_TEXT ){
      z = (char *)sqlite3_value_text(pVal);
    }
	/* sqlite3VdbeSetVarmask()��������SQL����iVar��ֵʹ�ð�һ����ֵ�����Ѻ���sqlite3_reoptimize(),
	������������ظ�׼��SQL������һ�����õĲ�ѯ�ƻ���*/  
    sqlite3VdbeSetVarmask(pParse->pVdbe, iCol);         
    assert( pRight->op==TK_VARIABLE || pRight->op==TK_REGISTER );
  }else if( op==TK_STRING ){
    z = pRight->u.zToken;  /*zToken��ʾ ���ֵ������ֹ��δ����*/
  }
  if( z ){
    cnt = 0;
    while( (c=z[cnt])!=0 && c!=wc[0] && c!=wc[1] && c!=wc[2] ){
      cnt++;
    }
    if( cnt!=0 && 255!=(u8)z[cnt-1] ){
      Expr *pPrefix;
      *pisComplete = c==wc[0] && z[cnt+1]==0;
      pPrefix = sqlite3Expr(db, TK_STRING, z);  /*SQL��������(TK_INTEGER,TK_FLOAT,TK_BLOB��TK_STRING)*/
      if( pPrefix ) pPrefix->u.zToken[cnt] = 0;
      *ppPrefix = pPrefix;
      if( op==TK_VARIABLE ){
        Vdbe *v = pParse->pVdbe;
        sqlite3VdbeSetVarmask(v, pRight->iColumn);
        if( *pisComplete && pRight->u.zToken[1] ){
          /* If the rhs of the LIKE expression is a variable, and the current
          ** value of the variable means there is no need to invoke the LIKE
          ** function, then no OP_Variable will be added to the program.
          ** This causes problems for the sqlite3_bind_parameter_name()
          ** API. To workaround them, add a dummy OP_Variable here.
          **
          ** ���LIKE���ʽ���ұ���һ�����������ҵ�ǰ������ֵ����û�б�Ҫ����LIKE��������ôOP_Variable���ᱻ��ӵ������С�
          ** �������sqlite3_bind_parameter_name() API��һЩ���⡣Ϊ�˽����Щ���⣬�������һ�������OP_Variable.
          **
          */ 
          int r1 = sqlite3GetTempReg(pParse);  /*����һ���µļĴ��������洢һЩ�м�����*/
		  /*���ɴ��뵽��ǰVDBE�������������ʽ��������洢�ڡ�target���Ĵ����С����ش洢����ļĴ�����*/
          sqlite3ExprCodeTarget(pParse, pRight, r1);
		  /*sqlite3VdbeChangeP3()����Ϊһ���ض���ָ��ı������P3��ֵ��*/
		  /*sqlite3VdbeCurrentAddr()�������ز�����һ��ָ��ĵ�ַ*/
          sqlite3VdbeChangeP3(v, sqlite3VdbeCurrentAddr(v)-1, 0);
		  /*�ͷ�һ���Ĵ�����ʹ���ܱ�����Ŀ������ʹ�á�*/
          sqlite3ReleaseTempReg(pParse, r1);
        }
      }
    }else{
      z = 0;
    }
  }

  sqlite3ValueFree(pVal);  /*�ͷ�sqlite3_value����*/
  return (z!=0);
}
#endif /* SQLITE_OMIT_LIKE_OPTIMIZATION */


#ifndef SQLITE_OMIT_VIRTUALTABLE
/*
** Check to see if the given expression is of the form
**
**         column MATCH expr
**
** If it is then return TRUE.  If not, return FALSE.
**
** �鿴���ʽ�Ƿ���column MATCH expr��ʽ��������򷵻�TRUE�����򷵻�FALSE
**
*/
static int isMatchOfColumn(
  Expr *pExpr      /* Test this expression ����������ʽ*/
){
  ExprList *pList;  /*����һ�����ʽ�б�ṹ���ʵ��*/

  if( pExpr->op!=TK_FUNCTION ){   /*TK_FUNCTION��ʾ���ʽ��һ��SQL����*/
    return 0;
  }
  /*sqlite3StrICmpΪ�궨�壬��ʾ�ڲ�����ԭ��*/
  if( sqlite3StrICmp(pExpr->u.zToken,"match")!=0 ){  /*zToken��ʾ ���ֵ������ֹ��δ����*/
    return 0;
  }
  /*
  union {
    ExprList *pList;      Function arguments or in "<expr> IN (<expr-list)" 	������������"<���ʽ>IN(<���ʽ�б�>)
    Select *pSelect;      Used for sub-selects and "<expr> IN (<select>)" 	������ѡ���"<���ʽ>IN(<ѡ��>)"
  } x;
  xΪExpr�ṹ�嶨���еĹ�ͬ�壬����sqliteInt.h 1845��
  */
  pList = pExpr->x.pList;
  if( pList->nExpr!=2 ){   /*nExpr��ʾ�б��б��ʽ����Ŀ*/
    return 0;
  }
  if( pList->a[1].pExpr->op != TK_COLUMN ){  /*TK_COLUMN��ʾ���ʽ��һ����*/
    return 0;
  }
  return 1;
}
#endif /* SQLITE_OMIT_VIRTUALTABLE */

/*
** If the pBase expression originated in the ON or USING clause of
** a join, then transfer the appropriate markings over to derived.
**
** ���pBase���ʽ��Դ��һ�����ӵ�ON��USING�Ӿ䣬��ô���ʵ������ӱ��ת�Ƹ������ı��ʽ pDerived��
**
*/
static void transferJoinMarkings(Expr *pDerived, Expr *pBase){
  pDerived->flags |= pBase->flags & EP_FromJoin;  /*EP_FromJoin��ʾ��Դ�����ӵ�ON��USING�Ӿ�*/
  pDerived->iRightJoinTable = pBase->iRightJoinTable;  /*iRightJoinTable��ʾ�����ӱ���*/
}

#if !defined(SQLITE_OMIT_OR_OPTIMIZATION) && !defined(SQLITE_OMIT_SUBQUERY)
/*
** Analyze a term that consists of two or more OR-connected
** subterms.  
** ��������������ʽ:
**
**     ... WHERE  (a=5) AND (b=7 OR c=9 OR d=13) AND (d=13)
**                          ^^^^^^^^^^^^^^^^^^^^
** ����һ���������������OR���ӵ���term��term��
**
** This routine analyzes terms such as the middle term in the above example.
** A WhereOrTerm object is computed and attached to the term under
** analysis, regardless of the outcome of the analysis.  Hence:
**
**     WhereTerm.wtFlags |= TERM_ORINFO
**     WhereTerm.u.pOrInfo = a dynamically allocated WhereOrTerm object  ��̬�ط���WhereOrTerm����
**
** �������������������ӵ��в�term��terms��
** һ��WhereOrTerm���󱻼��㣬�����ӵ�����������term�У����ܷ����Ľ����Ρ�
**
** The term being analyzed must have two or more of OR-connected subterms.
** A single subterm might be a set of AND-connected sub-subterms.
**
** ������term����������OR���ӵ���term��һ����������term������һ��AND���ӵ���term������������� C��D��E��
**
** Examples of terms under analysis(��������):
**
**     (A)     t1.x=t2.y OR t1.x=t2.z OR t1.y=15 OR t1.z=t3.a+5
**     (B)     x=expr1 OR expr2=x OR x=expr3
**     (C)     t1.x=t2.y OR (t1.x=t2.z AND t1.y=15)
**     (D)     x=expr1 OR (y>11 AND y<22 AND z LIKE '*hello*')
**     (E)     (p.a=1 AND q.b=2 AND r.c=3) OR (p.x=4 AND q.y=5 AND r.z=6)
**
** CASE 1:
**
** If all subterms are of the form T.C=expr for some single column of C
** a single table T (as shown in example B above) then create a new virtual
** term that is an equivalent IN expression.  In other words, if the term
** being analyzed is:
**
** ���������terms�Ƕ��ڵ���T��ͬһ�� C�ĵ�ʽ���ʽ T.C=expr��ʽ������������� B����
** �򴴽�һ���µ�����term����ͬ��IN���ʽ�����仰˵�������������term���£�  
**
**      x = expr1  OR  expr2 = x  OR  x = expr3
**
** then create a new virtual term like this:
** �򴴽�һ���µ�����term���£� 
**
**      x IN (expr1,expr2,expr3)
**
** CASE 2:  �����ͬһ���е��У��������"=", "<", "<=", ">", ">=", "IS NULL", or "IN"�������ʹ�������Ż�
**
** If all subterms are indexable by a single table T, then set
**
**     WhereTerm.eOperator              =  WO_OR
**     WhereTerm.u.pOrInfo->indexable  |=  the cursor number for table T   //T����α���
**
** A subterm is "indexable" if it is of the form
** "T.C <op> <expr>" where C is any column of table T and 
** <op> is one of "=", "<", "<=", ">", ">=", "IS NULL", or "IN".
** A subterm is also indexable if it is an AND of two or more
** subsubterms at least one of which is indexable.  Indexable AND 
** subterms have their eOperator set to WO_AND and they have
** u.pAndInfo set to a dynamically allocated WhereAndTerm object.
**
** ������е���terms���ǿɼ������ģ����Ҷ�����ͬһ����T����ô����
**     WhereTerm.eOperator              =  WO_OR
**     WhereTerm.u.pOrInfo->indexable  |=  the cursor number for table T  //T����α���
**  
** ���һ��term��"T.C <op> <expr>"����ʽ������C�Ǳ�T�е������У�
** <op>��"=", "<", "<=", ">", ">=", "IS NULL", or "IN"�е�һ����
** ��ô�����term��"indexable"��
** ���һ����term����һ��AND���ӵ��������������terms���������е���terms������һ����indexable��
** ��ô�����termҲ�ǿɼ������ġ�
** �ɼ�������AND��terms�����ǵ�eOperator����ΪWO_AND����u.pAndInfo����Ϊһ����̬�����WhereAndTerm����
**
** From another point of view, "indexable" means that the subterm could
** potentially be used with an index if an appropriate index exists.
** This analysis does not consider whether or not the index exists; that
** is something the bestIndex() routine will determine.  This analysis
** only looks at whether subterms appropriate for indexing exist.
**
** ����һ���濴��"�ɼ�������"��ζ�����һ���ʵ����������ڣ���ô��term����ͨ��һ��������ʹ�á�
** ������������������Ƿ���ڣ�����bestIndex()�����ǵ��¡��������ֻ��ע��terms�Ƿ��к��ʵ��������ڡ�
**
** All examples A through E above all satisfy case 2.  But if a term
** also statisfies case 1 (such as B) we know that the optimizer will
** always prefer case 1, so in that case we pretend that case 2 is not
** satisfied.
** 
** �����A��E�������Ӷ�����CASE 2�����ǣ����һ��termҲ����CASE 1�������� B����
** �����Ż�������������CASE 1����ˣ�����������£�������ΪCASE 2���ʺϡ�  
** ��������1�����2�����㣬��Ĭ��ʹ�����1�Ż���
**
** It might be the case that multiple tables are indexable.  For example,
** (E) above is indexable on tables P, Q, and R.  
** 
** ����������������ڶ�������ǿ������ģ������� E �ڱ�P��Q��R�϶���������
** 
** Terms that satisfy case 2 are candidates for lookup by using
** separate indices to find rowids for each subterm and composing
** the union of all rowids using a RowSet object.  This is similar
** to "bitmap indices" in other database engines.
**
** �������2��terms����ʹ�ø��Ե�����Ϊÿ��subterm�ҵ�rowid����ʹ��RowSet�����������rowid��
** ���������������ݿ�����ġ�bitmap indices�� 
**
** OTHERWISE:
**
** If neither case 1 nor case 2 apply, then leave the eOperator set to
** zero.  This term is not useful for search.
**
** ���� ����Ȳ�����CASE 1��Ҳ������CASE 2����eOperator����Ϊ0. ���term���ڲ�ѯ��û���õġ�  
*/
static void exprAnalyzeOrTerm(
  SrcList *pSrc,            /* the FROM clause  FROM�Ӿ� */
  WhereClause *pWC,         /* the complete WHERE clause  ������WHERE�Ӿ� */
  int idxTerm               /* Index of the OR-term to be analyzed  ����������OR-term���� */
){
  Parse *pParse = pWC->pParse;            /* Parser context ������������ */
  sqlite3 *db = pParse->db;               /* Database connection ���ݿ����� */
  WhereTerm *pTerm = &pWC->a[idxTerm];    /* The term to be analyzed Ҫ������term */
  Expr *pExpr = pTerm->pExpr;             /* The expression of the term  term�ı��ʽ */
  WhereMaskSet *pMaskSet = pWC->pMaskSet; /* Table use masks  ��ʹ�õ����� */
  int i;                                  /* Loop counters   ѭ�������� */
  WhereClause *pOrWc;       /* Breakup of pTerm into subterms  ��OR��������ֽ����terms��where�Ӿ� pTerm */
  WhereTerm *pOrTerm;       /* A Sub-term within the pOrWc  һ����pOrWc�е���term */
  WhereOrInfo *pOrInfo;     /* Additional information associated with pTerm  �루OR�������pTerm��صĸ�����Ϣ */
  Bitmask chngToIN;         /* Tables that might satisfy case 1  �������1�ı� */
  Bitmask indexable;        /* Tables that are indexable, satisfying case 2  �ɼ��������������2�ı� */

  /*
  ** Break the OR clause into its separate subterms.  The subterms are
  ** stored in a WhereClause structure containing within the WhereOrInfo
  ** object that is attached to the original OR clause term.
  **
  ** ��OR�Ӿ�ֽ�ɵ�������terms����terms�洢��һ��������WhereOrInfo�����WhereClause�ṹ�У�
  ** WhereOrInfo���󸽼ӵ�ԭʼ��OR�Ӿ�term�С�
  */
  /*
  ** wtFlags��ʾTERM_xxx bit��־��TERM_DYNAMIC��ʾ��Ҫ����sqlite3ExprDelete(db, pExpr)��
  ** TERM_ORINFO��ʾ��Ҫ�ͷ�WhereTerm.u.pOrInfo����TERM_ANDINFO��ʾ��Ҫ�ͷ�WhereTerm.u.pAndInfo����
  */
  assert( (pTerm->wtFlags & (TERM_DYNAMIC|TERM_ORINFO|TERM_ANDINFO))==0 );
  assert( pExpr->op==TK_OR );  /*�ж�Ϊ OR���ʽ*/
  pTerm->u.pOrInfo = pOrInfo = sqlite3DbMallocZero(db, sizeof(*pOrInfo));  /*�����ڴ�*/
  if( pOrInfo==0 ) return;  /*�����ڴ�ʧ�ܣ�����*/
  pTerm->wtFlags |= TERM_ORINFO;
  pOrWc = &pOrInfo->wc;  /* wc��ʾwhere�Ӿ�ֽ�Ϊ��terms */
  whereClauseInit(pOrWc, pWC->pParse, pMaskSet, pWC->wctrlFlags);  /*��ʼ��һ��Ԥ�����WhereClause���ݽṹ*/
  whereSplit(pOrWc, pExpr, TK_OR);  /*ʶ����WHERE�Ӿ��е��ӱ��ʽ������OR�����*/
  exprAnalyzeAll(pSrc, pOrWc);  /*���������Ӿ�*/
  if( db->mallocFailed ) return;  /*��̬�ڴ����ʧ�ܣ�����*/
  assert( pOrWc->nTerm>=2 );  /*�ж��Ӿ����term���ڵ���2����OR�����*/
  /*
  ** Compute the set of tables that might satisfy cases 1 or 2.
  **
  ** ��������������1�����2�ı�
  */
  indexable = ~(Bitmask)0;  /*�ɼ��������������2�ı�*/
  chngToIN = ~(pWC->vmask);  /*�������1�ı�*/
  /*�������� E ��ʽ*/
  for(i=pOrWc->nTerm-1, pOrTerm=pOrWc->a; i>=0 && indexable; i--, pOrTerm++){  /*ѭ��where�Ӿ��ÿ����term����term��OR���������ÿһ��*/
	/*WO_XX��ʾ�������λ���룬�������Խ��п�������Щֵ��OR-ed��Ͽ����ڲ���where�Ӿ����termsʱ��ʹ��.*/  
    if( (pOrTerm->eOperator & WO_SINGLE)==0 ){  /*WO_SINGLE��ʾ���е�һ��WO_XXֵ������*/
      WhereAndInfo *pAndInfo;  /*AND�����*/
      assert( pOrTerm->eOperator==0 );  /*eOperator��ʾ����<op>��һ��WO_XX*/
      assert( (pOrTerm->wtFlags & (TERM_ANDINFO|TERM_ORINFO))==0 );
      chngToIN = 0;  
      pAndInfo = sqlite3DbMallocRaw(db, sizeof(*pAndInfo));  /*�����ڴ�*/
      if( pAndInfo ){
        WhereClause *pAndWC;  /*��AND��������ֽ����terms��pTerm��pTermΪOR���������term*/
        WhereTerm *pAndTerm;  /*һ����pOrWc�е���term*/
        int j;  /*ѭ��������*/
        Bitmask b = 0;  
        pOrTerm->u.pAndInfo = pAndInfo;  /*�루AND�������pTerm��صĸ�����Ϣ*/
        pOrTerm->wtFlags |= TERM_ANDINFO;  /*��ֵΪTERM_ANDINFO��־*/
        pOrTerm->eOperator = WO_AND;  /*�������ֵΪWO_AND*/
        pAndWC = &pAndInfo->wc;
        whereClauseInit(pAndWC, pWC->pParse, pMaskSet, pWC->wctrlFlags);   /*��ʼ��һ��Ԥ�����WhereClause���ݽṹ*/
        whereSplit(pAndWC, pOrTerm->pExpr, TK_AND);  /*ʶ����WHERE�Ӿ��е��ӱ��ʽ������AND�����*/
        exprAnalyzeAll(pSrc, pAndWC);  /*���������Ӿ�*/
        pAndWC->pOuter = pWC;  /*������*/
        testcase( db->mallocFailed );  /*��testcase()���ڰ������ǲ��ԡ�*/
        if( !db->mallocFailed ){
          for(j=0, pAndTerm=pAndWC->a; j<pAndWC->nTerm; j++, pAndTerm++){  /*ѭ��OR�������ÿ����term����term��AND���������ÿһ��*/
            assert( pAndTerm->pExpr );  
            if( allowedOp(pAndTerm->pExpr->op) ){  /*�����"=", "<", "<=", ">", ">=", "IS NULL", or "IN"��һ��*/
              b |= getMask(pMaskSet, pAndTerm->leftCursor);  /*���ظ������α�����λ���룬AND����������α�*/
            }
          }
        }
        indexable &= b;  /*����������2�ı�*/
      }
    }else if( pOrTerm->wtFlags & TERM_COPIED ){  /*TERM_COPIED��ʾ��һ��child*/
      /* Skip this term for now.  We revisit it when we process the 
      ** corresponding TERM_VIRTUAL term 
	  **
	  ** ��ʱ�������term.��ִ�ж�Ӧ�ĵ�TERM_VIRTUAL termʱ�����·�������	
	  */
    }else{
      Bitmask b;
      b = getMask(pMaskSet, pOrTerm->leftCursor);  /*���ظ������α�����λ���룬OR����������α�*/
      if( pOrTerm->wtFlags & TERM_VIRTUAL ){  /*TERM_VIRTUAL��ʾ���Ż�����ӣ�����Ҫ����*/
        WhereTerm *pOther = &pOrWc->a[pOrTerm->iParent];  /*����where�Ӿ����term*/
        b |= getMask(pMaskSet, pOther->leftCursor);
      }
      indexable &= b;  /*����������2�ı�*/
      if( pOrTerm->eOperator!=WO_EQ ){  /*OR�������term���������λ���벻��WO_EQ*/
        chngToIN = 0;
      }else{
        chngToIN &= b;  /*����������1�ı�*/
      }
    }
  }

  /*
  ** Record the set of tables that satisfy case 2.  The set might be
  ** empty. 
  ** ��¼�������2�ı��������Ϊ�ա�
  */
  pOrInfo->indexable = indexable;
  pTerm->eOperator = indexable==0 ? 0 : WO_OR;  
  /*
  ** chngToIN holds a set of tables that *might* satisfy case 1.  But
  ** we have to do some additional checking to see if case 1 really
  ** is satisfied.
  ** 
  ** ���ֵ�һ�����ʱ�Ĵ���
  ** chngToIN��������������1�ı���������Ҫ��һЩ���Ӽ�鿴���ǲ�������������1
  **
  ** chngToIN will hold either 0, 1, or 2 bits.  
  ** The 0-bit case means that there is no possibility of transforming 
  ** the OR clause into an IN operator because one or more terms in the 
  ** OR clause contain something other than == on a column in the single table.  
  ** The 1-bit case means that every term of the OR clause is of the form
  ** "table.column=expr" for some single table.  The one bit that is set
  ** will correspond to the common table.  We still need to check to make
  ** sure the same column is used on all terms.  
  ** The 2-bit case is when the all terms are of the form "table1.column=table2.column".  
  ** It might be possible to form an IN operator with either table1.column or table2.column 
  ** as the LHS if either is common to every term of the OR clause.
  **
  ** chngToIN������0,1��2 bits.
  ** 0-bit�����ζ���޷���OR�Ӿ�ת��ΪIN�����(������B),��ΪOR�Ӿ��е�һ������terms�ڵ������һ���ϰ�����==��������������������
  ** 1-bit�����ζ��OR�Ӿ��ÿ��term����ĳ��������"table.column=expr"��ʽ�����ǻ���Ҫ��һ����֤����termsʹ�õ�ͬһ�С�
  ** 2-bit�����ζ�����е�terms����"table1.column=table2.column"��ʽ���������γ�һ��table1.column��table2.column��Ϊ��߲�������IN�������
  ** ����κ�һ����table1.column��table2.column����OR�Ӿ��ÿ��term���ǹ��õġ�
  **
  ** Note that terms of the form "table.column1=table.column2" (the
  ** same table on both sizes of the ==) cannot be optimized.
  ** 
  ** ע��:"table.column1=table.column2"(��==�����߶���ͬһ����)��ʽ�ǲ����Ż���
  */
  if( chngToIN ){  /*�������1*/
    int okToChngToIN = 0;     /* True if the conversion to IN is valid  �������ת��ΪIN��������ΪTRUE */
    int iColumn = -1;         /* Column index on lhs of IN operator  IN�������ߵ������� */
    int iCursor = -1;         /* Table cursor common to all terms  ����terms��ͬ�ı��α� */
    int j = 0;                /* Loop counter  ѭ�������� */

    /* Search for a table and column that appears on one side or the
    ** other of the == operator in every subterm.  That table and column
    ** will be recorded in iCursor and iColumn.  There might not be any
    ** such table and column.  Set okToChngToIN if an appropriate table
    ** and column is found but leave okToChngToIN false if not found.
    **
    ** ����һ������У���������ÿ����term��==�����������һ�ߡ��������б���¼��iCursor��iColumn�С�
    ** Ҳ����û���κ������ı���С����һ���ʵ��ı���б����ҵ���������okToChngToINΪTRUE����������okToChngToINΪFALSE��
    */
    for(j=0; j<2 && !okToChngToIN; j++){
      pOrTerm = pOrWc->a;  /*where�Ӿ�OR������ָ���һ��term*/
      for(i=pOrWc->nTerm-1; i>=0; i--, pOrTerm++){
        assert( pOrTerm->eOperator==WO_EQ );  /*�ж�term�����ΪWO_EQ*/
        pOrTerm->wtFlags &= ~TERM_OR_OK;  /*OR�����*/
        if( pOrTerm->leftCursor==iCursor ){  /*iCursor��ʾ�������1�ı�*/
          /* This is the 2-bit case and we are on the second iteration and   	
          ** current term is from the first iteration.  So skip this term. 
		  **
		  ** ����2-bit��������������ڵڶ��ε����£���ǰterm�������ڵ�һ�ε����������������term��	
		  */
          assert( j==1 );
          continue;
        }
        if( (chngToIN & getMask(pMaskSet, pOrTerm->leftCursor))==0 ){
          /* This term must be of the form t1.a==t2.b where t2 is in the 
          ** chngToIN set but t1 is not.  This term will be either preceeded or
          ** follwed by an inverted copy (t2.b==t1.a).  Skip this term and use its inversion. 
		  **
		  ** ���term������t1.a==t2.b��ʽ������t2��chngToIN�еı���t1���ǡ�
		  ** ���term����ִ�л���з�ת����(t2.b==t1.a)���������term��ʹ�����ķ�ת��ʽ��	
		  */
          testcase( pOrTerm->wtFlags & TERM_COPIED );  /*��testcase()���ڰ������ǲ��ԡ�*/
          testcase( pOrTerm->wtFlags & TERM_VIRTUAL );
          assert( pOrTerm->wtFlags & (TERM_COPIED|TERM_VIRTUAL) );
          continue;
        }
        iColumn = pOrTerm->u.leftColumn;  /*ȷ����*/
        iCursor = pOrTerm->leftCursor;  /*ȷ����*/
        break;
      }
      if( i<0 ){
        /* No candidate table+column was found.  This can only occur on the second iteration.
        **  
		** û���ҵ����������ĺ�ѡ����С���ֻ�ܷ����ڵڶ���ѭ����
		*/
        assert( j==1 );
        assert( (chngToIN&(chngToIN-1))==0 );
        assert( chngToIN==getMask(pMaskSet, iCursor) );
        break;
      }
      testcase( j==1 );

      /* We have found a candidate table and column.  Check to see if that 
      ** table and column is common to every term in the OR clause 
	  ** 
	  ** �Ѿ�������һ����ѡ����С��ٲ鿴������Ƿ��OR�Ӿ��е�����term���ǹ�ͬ�ġ�
	  */
      okToChngToIN = 1;
      for(; i>=0 && okToChngToIN; i--, pOrTerm++){
        assert( pOrTerm->eOperator==WO_EQ );
        if( pOrTerm->leftCursor!=iCursor ){  /*����*/
          pOrTerm->wtFlags &= ~TERM_OR_OK;  /*��TERM_OR_OK*/
        }else if( pOrTerm->u.leftColumn!=iColumn ){  /*�в���*/
          okToChngToIN = 0;  /*��ѡ����в��ǣ�����ת��ΪIN������*/
        }else{
          int affLeft, affRight;
          /* If the right-hand side is also a column, then the affinities  
          ** of both right and left sides must be such that no type 
          ** conversions are required on the right.  (Ticket #2249)
          ** 
		  ** ����ұ�Ҳ��һ���У���ô�������ߵĹ������Ǳ���������ģ��ұ߲���Ҫ����ת����
		  */
          affRight = sqlite3ExprAffinity(pOrTerm->pExpr->pRight);  /*���ر��ʽpExpr���ұߴ��ڵĹ����� 'affinity'*/
          affLeft = sqlite3ExprAffinity(pOrTerm->pExpr->pLeft);  /*���ر��ʽpExpr����ߴ��ڵĹ����� 'affinity'*/
          if( affRight!=0 && affRight!=affLeft ){  /*�����������*/
            okToChngToIN = 0;
          }else{
            pOrTerm->wtFlags |= TERM_OR_OK;  /*�й�����*/
          }
        }
      }
    }

    /* At this point, okToChngToIN is true if original pTerm satisfies case 1.
    ** In that case, construct a new virtual term that is pTerm converted into an IN operator.
    **
    ** ��ʱ�����ԭʼ��pTerm�������1����okToChngToINΪTRUE����������£���Ҫ����һ���µ������term����pTermת��ΪIN��������
    */
    if( okToChngToIN ){
      Expr *pDup;            /* A transient duplicate expression  һ����ʱ�ĸ��Ʊ��ʽ */
      ExprList *pList = 0;   /* The RHS of the IN operator  IN�������ұ� */
      Expr *pLeft = 0;       /* The LHS of the IN operator  IN��������� */
      Expr *pNew;            /* The complete IN operator  ������IN������ */

      for(i=pOrWc->nTerm-1, pOrTerm=pOrWc->a; i>=0; i--, pOrTerm++){
        if( (pOrTerm->wtFlags & TERM_OR_OK)==0 ) continue;
        assert( pOrTerm->eOperator==WO_EQ );
        assert( pOrTerm->leftCursor==iCursor );
        assert( pOrTerm->u.leftColumn==iColumn );  /*�ж����������1�ı���У�������*/
        pDup = sqlite3ExprDup(db, pOrTerm->pExpr->pRight, 0);  /*���Ʊ��ʽ�ұߡ� ����expr.c 900��*/
		/*�ڱ��ʽ�б�ĩβ���һ���µ�Ԫ�ء����pList����ǿյģ��򴴽�һ���µı��ʽ�б�*/
        pList = sqlite3ExprListAppend(pWC->pParse, pList, pDup);
        pLeft = pOrTerm->pExpr->pLeft;
      }
      assert( pLeft!=0 );
      pDup = sqlite3ExprDup(db, pLeft, 0);  /*���Ʊ��ʽ���*/
      pNew = sqlite3PExpr(pParse, TK_IN, pDup, 0, 0);  /*����һ��Expr���ʽ�ڵ㣬�������������ڵ㡣 ����expr.c 496��*/
      if( pNew ){  /*������IN���������Ƴɹ�*/
        int idxNew;  /*��������*/
        transferJoinMarkings(pNew, pExpr);
        assert( !ExprHasProperty(pNew, EP_xIsSelect) );  /*ExprHasProperty����Expr.flags�ֶ����ڲ���*/
        pNew->x.pList = pList;
        idxNew = whereClauseInsert(pWC, pNew, TERM_VIRTUAL|TERM_DYNAMIC);  /*������WhereTerm��pWC->a[]������*/
        testcase( idxNew==0 );  /*���ǲ���*/
        exprAnalyze(pSrc, pWC, idxNew);  /*�����ӱ��ʽ����whereterm*/
        pTerm = &pWC->a[idxTerm];  
        pWC->a[idxNew].iParent = idxTerm;
        pTerm->nChild = 1;
      }else{
        sqlite3ExprListDelete(db, pList);  /*ɾ�������б�*/
      }
      pTerm->eOperator = WO_NOOP;   /* case 1 trumps case 2  ���1ʤ�����2��  WO_NOOP��ʾ���term�����������ռ�*/
    }
  }
}
#endif /* !SQLITE_OMIT_OR_OPTIMIZATION && !SQLITE_OMIT_SUBQUERY */


/*
** The input to this routine is an WhereTerm structure with only the
** "pExpr" field filled in.  The job of this routine is to analyze the
** subexpression and populate all the other fields of the WhereTerm
** structure.
**
** ��������������һ��ֻ��"pExpr"�ֶα�����WhereTerm���ݽṹ��
** �������������Ƿ����ӱ��ʽ�����WhereTerm���ݽṹ�������ֶΡ�
**
** If the expression is of the form "<expr> <op> X" it gets commuted
** to the standard form of "X <op> <expr>".
**
** ������ʽ��"<expr> <op> X"��ʽ������ת��Ϊ��׼��ʽ"X <op> <expr>".
**
** If the expression is of the form "X <op> Y" where both X and Y are
** columns, then the original expression is unchanged and a new virtual
** term of the form "Y <op> X" is added to the WHERE clause and
** analyzed separately.  The original term is marked with TERM_COPIED
** and the new term is marked with TERM_DYNAMIC (because it's pExpr
** needs to be freed with the WhereClause) and TERM_VIRTUAL (because it
** is a commuted copy of a prior term.)  The original term has nChild=1
** and the copy has idxParent set to the index of the original term.
**
** ������ʽ��"X <op> Y"��ʽ������X��Y�����У�
** Ȼ��ԭʼ�ı��ʽ����ı䲢�һ���WHERE�Ӿ������һ���µ������term--"Y <op> X"���ҷֱ𱻷�����
** ԭʼ��term������TERM_COPIED�������µ�term������TERM_DYNAMIC(��Ϊ����pExpr����Ҫ��WhereClause���ͷ�)
** ��TERM_VIRTUAL(��Ϊ��һ����ǰ��term�ĸ���)��
** ԭʼ��term��nChild=1�����Ҹ�����idxParent�����Ұ�idxParent����Ϊԭʼterm���±�
*/
static void exprAnalyze(
  SrcList *pSrc,            /* the FROM clause FROM�Ӿ� */
  WhereClause *pWC,         /* the WHERE clause WHERE�Ӿ� */
  int idxTerm               /* Index of the term to be analyzed ��Ҫ������term�±� */
){
  WhereTerm *pTerm;                /* The term to be analyzed ��Ҫ������term  */
  WhereMaskSet *pMaskSet;          /* Set of table index masks ���ñ��������� */
  Expr *pExpr;                     /* The expression to be analyzed ��Ҫ�����ı��ʽ */
  Bitmask prereqLeft;              /* Prerequesites of the pExpr->pLeft pExpr->pLeft��ǰ������  */
  Bitmask prereqAll;               /* Prerequesites of pExpr pExpr��ǰ������ */
  Bitmask extraRight = 0;          /* Extra dependencies on LEFT JOIN ���������еĶ�������� */
  Expr *pStr1 = 0;                 /* RHS of LIKE/GLOB operator LIKE/GLOB��������ұ� */
  int isComplete = 0;              /* RHS of LIKE/GLOB ends with wildcard LIKE/GLOB�ұ�����ͨ������� */
  int noCase = 0;                  /* LIKE/GLOB distinguishes case LIKE/GLOB���ִ�Сд */
  int op;                          /* Top-level operator.  pExpr->op */
  Parse *pParse = pWC->pParse;     /* Parsing context ���������� */
  sqlite3 *db = pParse->db;        /* Database connection ���ݿ����� */

  if( db->mallocFailed ){
    return;
  }
  pTerm = &pWC->a[idxTerm];
  pMaskSet = pWC->pMaskSet;
  pExpr = pTerm->pExpr;
  prereqLeft = exprTableUsage(pMaskSet, pExpr->pLeft);
  op = pExpr->op;
  if( op==TK_IN ){
    assert( pExpr->pRight==0 );
    if( ExprHasProperty(pExpr, EP_xIsSelect) ){
      pTerm->prereqRight = exprSelectTableUsage(pMaskSet, pExpr->x.pSelect);
    }else{
      pTerm->prereqRight = exprListTableUsage(pMaskSet, pExpr->x.pList);
    }
  }else if( op==TK_ISNULL ){
    pTerm->prereqRight = 0;
  }else{
    pTerm->prereqRight = exprTableUsage(pMaskSet, pExpr->pRight);
  }
  prereqAll = exprTableUsage(pMaskSet, pExpr);
  if( ExprHasProperty(pExpr, EP_FromJoin) ){
    Bitmask x = getMask(pMaskSet, pExpr->iRightJoinTable);
    prereqAll |= x;
    extraRight = x-1;  /* ON clause terms may not be used with an index �������ӵ�����е�ON�Ӿ�terms���ܲ���������һ��ʹ��
                       ** on left table of a LEFT JOIN.  Ticket #3015 */
  }
  pTerm->prereqAll = prereqAll;
  pTerm->leftCursor = -1;
  pTerm->iParent = -1;
  pTerm->eOperator = 0;
  if( allowedOp(op) && (pTerm->prereqRight & prereqLeft)==0 ){
    Expr *pLeft = pExpr->pLeft;
    Expr *pRight = pExpr->pRight;
    if( pLeft->op==TK_COLUMN ){
      pTerm->leftCursor = pLeft->iTable;
      pTerm->u.leftColumn = pLeft->iColumn;
      pTerm->eOperator = operatorMask(op);
    }
    if( pRight && pRight->op==TK_COLUMN ){
      WhereTerm *pNew;
      Expr *pDup;
      if( pTerm->leftCursor>=0 ){
        int idxNew;
        pDup = sqlite3ExprDup(db, pExpr, 0);
        if( db->mallocFailed ){
          sqlite3ExprDelete(db, pDup);
          return;
        }
        idxNew = whereClauseInsert(pWC, pDup, TERM_VIRTUAL|TERM_DYNAMIC);
        if( idxNew==0 ) return;
        pNew = &pWC->a[idxNew];
        pNew->iParent = idxTerm;
        pTerm = &pWC->a[idxTerm];
        pTerm->nChild = 1;
        pTerm->wtFlags |= TERM_COPIED;
      }else{
        pDup = pExpr;
        pNew = pTerm;
      }
      exprCommute(pParse, pDup);
      pLeft = pDup->pLeft;
      pNew->leftCursor = pLeft->iTable;
      pNew->u.leftColumn = pLeft->iColumn;
      testcase( (prereqLeft | extraRight) != prereqLeft );
      pNew->prereqRight = prereqLeft | extraRight;
      pNew->prereqAll = prereqAll;
      pNew->eOperator = operatorMask(pDup->op);
    }
  }

#ifndef SQLITE_OMIT_BETWEEN_OPTIMIZATION
  /* If a term is the BETWEEN operator, create two new virtual terms
  ** that define the range that the BETWEEN implements.  For example:
  **
  **      a BETWEEN b AND c
  **
  ** is converted into:
  **
  **      (a BETWEEN b AND c) AND (a>=b) AND (a<=c)
  **
  ** The two new terms are added onto the end of the WhereClause object.
  ** The new terms are "dynamic" and are children of the original BETWEEN
  ** term.  That means that if the BETWEEN term is coded, the children are
  ** skipped.  Or, if the children are satisfied by an index, the original
  ** BETWEEN term is skipped.
<<<<<<< HEAD
  ** BETWEEN��䴦����
=======
  **
  ** ���һ��term��BETWEEN����������������µ�����terms������BETWEEN�ķ�Χ��
  ** ����:
  **      a BETWEEN b AND c
  ** ת��Ϊ:
  **      (a BETWEEN b AND c) AND (a>=b) AND (a<=c)
  ** �����µ�terms����ӵ�WhereClause��������
  ** �µ�terms��"dynamic"������ԭʼBETWEEN term����term.
  ** ����ζ�����BETWEEN term�Ѿ����룬��ô����term��������.
  ** ���ߣ��������term�������ʹ����������ôԭʼ��BETWEEN term����������
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  else if( pExpr->op==TK_BETWEEN && pWC->op==TK_AND ){
    ExprList *pList = pExpr->x.pList;
    int i;
    static const u8 ops[] = {TK_GE, TK_LE};
    assert( pList!=0 );
    assert( pList->nExpr==2 );
    for(i=0; i<2; i++){
      Expr *pNewExpr; //���ڱ���ת������±��ʽ
      int idxNew;  //�²���ı��ʽ����WhereClause�е��±�
      pNewExpr = sqlite3PExpr(pParse, ops[i], 	//����>=��<=���ʽ
                             sqlite3ExprDup(db, pExpr->pLeft, 0),
                             sqlite3ExprDup(db, pList->a[i].pExpr, 0), 0);
      idxNew = whereClauseInsert(pWC, pNewExpr, TERM_VIRTUAL|TERM_DYNAMIC);
      testcase( idxNew==0 ); //����Ƿ�ת���ɹ�
      exprAnalyze(pSrc, pWC, idxNew);
      pTerm = &pWC->a[idxTerm];
      pWC->a[idxNew].iParent = idxTerm; //��ʾ�½����Ӿ���between���Ӿ��ת��
    }
    pTerm->nChild = 2;
  }
#endif /* SQLITE_OMIT_BETWEEN_OPTIMIZATION */

#if !defined(SQLITE_OMIT_OR_OPTIMIZATION) && !defined(SQLITE_OMIT_SUBQUERY)
  /* Analyze a term that is composed of two or more subterms connected by
  ** an OR operator.  ������OR��������ӵ�������������terms���
  */
  else if( pExpr->op==TK_OR ){  //����ñ��ʽ����OR���������
    assert( pWC->op==TK_AND ); //���where�Ӿ�����and�ָ���
    exprAnalyzeOrTerm(pSrc, pWC, idxTerm); //�Ż����Ӿ�
    pTerm = &pWC->a[idxTerm];
  }
#endif /* SQLITE_OMIT_OR_OPTIMIZATION */

#ifndef SQLITE_OMIT_LIKE_OPTIMIZATION
  /* Add constraints to reduce the search space on a LIKE or GLOB
  ** operator.
  **
  ** A like pattern of the form "x LIKE 'abc%'" is changed into constraints
  **
  **          x>='abc' AND x<'abd' AND x LIKE 'abc%'
  **
  ** The last character of the prefix "abc" is incremented to form the
  ** termination condition "abd".
<<<<<<< HEAD
  ** LIKE��䴦����
=======
  **
  ** ���Լ��������LIKE��GLOB������������ռ䡣
  ** likeģʽ"x LIKE 'abc%'"����ת��ΪԼ��
  **          x>='abc' AND x<'abd' AND x LIKE 'abc%'
  ** ǰ׺"abc"�����һ���ַ�һֱ���ӣ���������Ϊ"abd".
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pWC->op==TK_AND 
   && isLikeOrGlob(pParse, pExpr, &pStr1, &isComplete, &noCase) //�ж��Ƿ��ǿ����Ż���LIKE��GLOB����
  ){
    Expr *pLeft;       /* LHS of LIKE/GLOB operator LIKE/GLOB���������� */
    Expr *pStr2;       /* Copy of pStr1 - RHS of LIKE/GLOB operator LIKE/GLOB�������pStr1 - RHS�ĸ��� */
    Expr *pNewExpr1;
    Expr *pNewExpr2;
    int idxNew1;
    int idxNew2;
    CollSeq *pColl;    /* Collating sequence to use ʹ���������� */

    pLeft = pExpr->x.pList->a[1].pExpr;
    pStr2 = sqlite3ExprDup(db, pStr1, 0);
    if( !db->mallocFailed ){ //�����ʼ���ɹ�
      u8 c, *pC;       /* Last character before the first wildcard �ڵ�һ��ͨ���ǰ�����һ���ַ� */
      pC = (u8*)&pStr2->u.zToken[sqlite3Strlen30(pStr2->u.zToken)-1];
      c = *pC;
      if( noCase ){  //���like��glob���ִ�Сд
        /* The point is to increment the last character before the first
        ** wildcard.  But if we increment '@', that will push it into the
        ** alphabetic range where case conversions will mess up the 
        ** inequality.  To avoid this, make sure to also run the full
        ** LIKE on all candidate expressions by clearing the isComplete flag
        **
        ** Ŀ�����ڵ�һ��ͨ���ǰ�������һ���ַ���
        ** �������������'@',�ǽ�������Ƴ���ĸ��ķ�Χ����ô�ַ�ת�������벻ƽ�ȵĻ��ҡ�
        ** Ϊ�˱������������ʹ�����isComplete��־��ȷ�������к�ѡ���ʽ��Ҳ����������LIKE
        **
        */
        if( c=='A'-1 ) isComplete = 0; /* EV: R-64339-08207 */


        c = sqlite3UpperToLower[c];
      }
      *pC = c + 1;	//����<���ʽ���ַ��������һ���ַ�
    }
    pColl = sqlite3FindCollSeq(db, SQLITE_UTF8, noCase ? "NOCASE" : "BINARY",0); //����ķ�ʽ
    pNewExpr1 = sqlite3PExpr(pParse, TK_GE, 
                     sqlite3ExprSetColl(sqlite3ExprDup(db,pLeft,0), pColl),
                     pStr1, 0);  //����>=���ʽ
    idxNew1 = whereClauseInsert(pWC, pNewExpr1, TERM_VIRTUAL|TERM_DYNAMIC); //����>=���ʽ��where�Ӿ���
    testcase( idxNew1==0 ); //�����Ƿ����ɹ�
    exprAnalyze(pSrc, pWC, idxNew1); //�������ʽ��ʽ�Ƿ�Ϊx <op> <expr>�����������ת��Ϊ������ʽ
    pNewExpr2 = sqlite3PExpr(pParse, TK_LT,
                     sqlite3ExprSetColl(sqlite3ExprDup(db,pLeft,0), pColl),
                     pStr2, 0);  //����<���ʽ
    idxNew2 = whereClauseInsert(pWC, pNewExpr2, TERM_VIRTUAL|TERM_DYNAMIC); //����<���ʽ��where�Ӿ���
    testcase( idxNew2==0 ); //�����Ƿ����ɹ�
    exprAnalyze(pSrc, pWC, idxNew2); //�������ʽ��ʽ�Ƿ�Ϊx <op> <expr>�����������ת��Ϊ������ʽ
    pTerm = &pWC->a[idxTerm];
    if( isComplete ){  //���like��glob�ұ�����ͨ��������������´������Ӿ�����like���
      pWC->a[idxNew1].iParent = idxTerm;
      pWC->a[idxNew2].iParent = idxTerm;
      pTerm->nChild = 2;
    }
  }
#endif /* SQLITE_OMIT_LIKE_OPTIMIZATION */

#ifndef SQLITE_OMIT_VIRTUALTABLE
  /* Add a WO_MATCH auxiliary term to the constraint set if the
  ** current expression is of the form:  column MATCH expr.
  ** This information is used by the xBestIndex methods of
  ** virtual tables.  The native query optimizer does not attempt
  ** to do anything with MATCH functions.
  **
  ** �����ǰ���ʽ��column MATCH expr��ʽʱ�����һ��WO_MATCH����termԼ�����ϡ�
  ** �����Ϣ��ͨ������xBestIndex����ʹ�õġ����صĲ�ѯ�Ż�������ʹ��MATCH�������κ�����
  */
  if( isMatchOfColumn(pExpr) ){
    int idxNew;
    Expr *pRight, *pLeft;
    WhereTerm *pNewTerm;
    Bitmask prereqColumn, prereqExpr;

    pRight = pExpr->x.pList->a[0].pExpr;
    pLeft = pExpr->x.pList->a[1].pExpr;
    prereqExpr = exprTableUsage(pMaskSet, pRight);
    prereqColumn = exprTableUsage(pMaskSet, pLeft);
    if( (prereqExpr & prereqColumn)==0 ){
      Expr *pNewExpr;
      pNewExpr = sqlite3PExpr(pParse, TK_MATCH, 
                              0, sqlite3ExprDup(db, pRight, 0), 0);
      idxNew = whereClauseInsert(pWC, pNewExpr, TERM_VIRTUAL|TERM_DYNAMIC);
      testcase( idxNew==0 );
      pNewTerm = &pWC->a[idxNew];
      pNewTerm->prereqRight = prereqExpr;
      pNewTerm->leftCursor = pLeft->iTable;
      pNewTerm->u.leftColumn = pLeft->iColumn;
      pNewTerm->eOperator = WO_MATCH;
      pNewTerm->iParent = idxTerm;
      pTerm = &pWC->a[idxTerm];
      pTerm->nChild = 1;
      pTerm->wtFlags |= TERM_COPIED;
      pNewTerm->prereqAll = pTerm->prereqAll;
    }
  }
#endif /* SQLITE_OMIT_VIRTUALTABLE */

#ifdef SQLITE_ENABLE_STAT3
  /* When sqlite_stat3 histogram data is available an operator of the
  ** form "x IS NOT NULL" can sometimes be evaluated more efficiently
  ** as "x>NULL" if x is not an INTEGER PRIMARY KEY.  So construct a
  ** virtual term of that form.
  **
  ** Note that the virtual term must be tagged with TERM_VNULL.  This
  ** TERM_VNULL tag will suppress the not-null check at the beginning
  ** of the loop.  Without the TERM_VNULL flag, the not-null check at
  ** the start of the loop will prevent any results from being returned.
  **
  ** ��sqlite_stat3ֱ��ͼ��������Ч��һ��"x IS NOT NULL"��ʽ���������
  ** ���x����INTEGER PRIMARY KEY����ô"x IS NOT NULL"��ʽ����Ϊ'x>NULL'�Ǹ�����Ч�ġ����Թ���һ���Ǹ���ʽ������term
  ** ע��:�����term������ΪTERM_VNULL.TERM_VNULL��ǽ���ѭ��һ��ʼ�ͷ�ֹnot-null��顣
  ** û��TERM_VNULL��־����ѭ����ʼ��not-null��齫��ֹ��ν���ķ��ء�
  **
  */
  if( pExpr->op==TK_NOTNULL
   && pExpr->pLeft->op==TK_COLUMN
   && pExpr->pLeft->iColumn>=0
  ){
    Expr *pNewExpr;
    Expr *pLeft = pExpr->pLeft;
    int idxNew;
    WhereTerm *pNewTerm;

    pNewExpr = sqlite3PExpr(pParse, TK_GT,
                            sqlite3ExprDup(db, pLeft, 0),
                            sqlite3PExpr(pParse, TK_NULL, 0, 0, 0), 0);

    idxNew = whereClauseInsert(pWC, pNewExpr,
                              TERM_VIRTUAL|TERM_DYNAMIC|TERM_VNULL);
    if( idxNew ){
      pNewTerm = &pWC->a[idxNew];
      pNewTerm->prereqRight = 0;
      pNewTerm->leftCursor = pLeft->iTable;
      pNewTerm->u.leftColumn = pLeft->iColumn;
      pNewTerm->eOperator = WO_GT;
      pNewTerm->iParent = idxTerm;
      pTerm = &pWC->a[idxTerm];
      pTerm->nChild = 1;
      pTerm->wtFlags |= TERM_COPIED;
      pNewTerm->prereqAll = pTerm->prereqAll;
    }
  }
#endif /* SQLITE_ENABLE_STAT */

  /* Prevent ON clause terms of a LEFT JOIN from being used to drive
  ** an index for tables to the left of the join.
  **
  ** ��ֹһ�������ӵ�ON�Ӿ�terms��������һ��������������ӵ����
  */
  pTerm->prereqRight |= extraRight;
}

/*
** Return TRUE if any of the expressions in pList->a[iFirst...] contain
** a reference to any table other than the iBase table.
**
** �����pList->a[iFirst...]�е��κεı��ʽ�����iBase��֮����κα��й�������ô����TRUE
**
*/
static int referencesOtherTables(
  ExprList *pList,          /* Search expressions in ths list �����list�в��ұ��ʽ */
  WhereMaskSet *pMaskSet,   /* Mapping from tables to bitmaps ���λͼ֮���ӳ�� */
  int iFirst,               /* Be searching with the iFirst-th expression ��iFirst-thһ���������ʽ */
  int iBase                 /* Ignore references to this table ���Զ����������� */
){
  Bitmask allowed = ~getMask(pMaskSet, iBase);
  while( iFirst<pList->nExpr ){
    if( (exprTableUsage(pMaskSet, pList->a[iFirst++].pExpr)&allowed)!=0 ){
      return 1;
    }
  }
  return 0;
}

/*
** This function searches the expression list passed as the second argument
** for an expression of type TK_COLUMN that refers to the same column and
** uses the same collation sequence as the iCol'th column of index pIdx.
** Argument iBase is the cursor number used for the table that pIdx refers
** to.
**
** ���������ѯ���ʽ�б���Ϊ�ڶ����������ݸ�����TK_COLUMN�ı��ʽ��
** ���ʽ������ͬ���в���ʹ����ͬ������������Ϊ����pIdx��iCol'th�С�
** ����iBase�����α���������pIdx���õı��ϡ�
**
** If such an expression is found, its index in pList->a[] is returned. If
** no expression is found, -1 is returned.
**
** ��һ�����ʽ���鵽������������pList->a[]�±ꡣ���û�в鵽���򷵻�-1.
*/
static int findIndexCol(
  Parse *pParse,                  /* Parse context ���������� */
  ExprList *pList,                /* Expression list to search ���ڲ��ҵı��ʽ�б� */
  int iBase,                      /* Cursor for table associated with pIdx ��pIdx��صı��α� */
  Index *pIdx,                    /* Index to match column of ��ƥ��������� */
  int iCol                        /* Column of index to match ƥ��������� */
){
  int i;
  const char *zColl = pIdx->azColl[iCol];

  for(i=0; i<pList->nExpr; i++){
    Expr *p = pList->a[i].pExpr;
    if( p->op==TK_COLUMN
     && p->iColumn==pIdx->aiColumn[iCol]
     && p->iTable==iBase
    ){
      CollSeq *pColl = sqlite3ExprCollSeq(pParse, p);
      if( ALWAYS(pColl) && 0==sqlite3StrICmp(pColl->zName, zColl) ){
        return i;
      }
    }
  }

  return -1;
}

/*
** This routine determines if pIdx can be used to assist in processing a
** DISTINCT qualifier. In other words, it tests whether or not using this
** index for the outer loop guarantees that rows with equal values for
** all expressions in the pDistinct list are delivered grouped together.
**
** For example, the query 
**
**   SELECT DISTINCT a, b, c FROM tbl WHERE a = ?
**
** can benefit from any index on columns "b" and "c".
**
** �������������pIdx�ܱ����ڸ����ڳ���ִ���е�DISTINCT�޶���
** ���仰˵���������Ƿ�Ϊ���ⲿѭ��ʹ�������������֤��pDistinct�б������б��ʽ�е�ֵ�����������һ�𽻸���
*/
static int isDistinctIndex(
  Parse *pParse,                  /* Parsing context ���������� */
  WhereClause *pWC,               /* The WHERE clause WHERE�Ӿ� */
  Index *pIdx,                    /* The index being considered �����ǵ����� */
  int base,                       /* Cursor number for the table pIdx is on pIdxʹ�õı��α��� */
  ExprList *pDistinct,            /* The DISTINCT expressions DISTINCT���ʽ */
  int nEqCol                      /* Number of index columns with == ==�������е���Ŀ */
){
  Bitmask mask = 0;               /* Mask of unaccounted for pDistinct exprs δ���͵�pDistinct exprs���� */
  int i;                          /* Iterator variable �������� */

  if( pIdx->zName==0 || pDistinct==0 || pDistinct->nExpr>=BMS ) return 0;
  testcase( pDistinct->nExpr==BMS-1 );

  /* Loop through all the expressions in the distinct list. If any of them
  ** are not simple column references, return early. Otherwise, test if the
  ** WHERE clause contains a "col=X" clause. If it does, the expression
  ** can be ignored. If it does not, and the column does not belong to the
  ** same table as index pIdx, return early. Finally, if there is no
  ** matching "col=X" expression and the column is on the same table as pIdx,
  ** set the corresponding bit in variable mask.
  **
  ** ѭ��������distinct list�е����еı��ʽ����������е��κ�һ�����Ǽ򵥵������ã�����ǰ���ء�
  ** �������WHERE�Ӿ����"col=X"�Ӿ䣬��ô�Ͳ��ԡ�
  ** ������������ʽ���ܱ����ԡ����û�а�������������Ϊ����pIdxҲ������ͬһ����ǰ���ء�
  ** ������û����"col=X"���ʽƥ�䲢������ΪpIdx����ͬһ���У��ڱ���������������Ӧ��λ
  **
  */
  for(i=0; i<pDistinct->nExpr; i++){
    WhereTerm *pTerm;
    Expr *p = pDistinct->a[i].pExpr;
    if( p->op!=TK_COLUMN ) return 0;
    pTerm = findTerm(pWC, p->iTable, p->iColumn, ~(Bitmask)0, WO_EQ, 0);
    if( pTerm ){
      Expr *pX = pTerm->pExpr;
      CollSeq *p1 = sqlite3BinaryCompareCollSeq(pParse, pX->pLeft, pX->pRight);
      CollSeq *p2 = sqlite3ExprCollSeq(pParse, p);
      if( p1==p2 ) continue;
    }
    if( p->iTable!=base ) return 0;
    mask |= (((Bitmask)1) << i);
  }

  for(i=nEqCol; mask && i<pIdx->nColumn; i++){
    int iExpr = findIndexCol(pParse, pDistinct, base, pIdx, i);
    if( iExpr<0 ) break;
    mask &= ~(((Bitmask)1) << iExpr);
  }

  return (mask==0);
}



/*�ϸӱ�ʼ
** Return true if the DISTINCT expression-list passed as the third argument
** is redundant. A DISTINCT list is redundant if the database contains a
** UNIQUE index that guarantees that the result of the query will be distinct
** anyway.
**
** ���DISTINCT���ʽlist���������������Ƕ���ģ��ͷ���true.
** ������ݿ����һ��UNIQUE������֤��ѯ�Ľ��Ҳ����Ψһ�ģ���ôһ��DISTINCT list�Ƕ���ģ�
*/
/* ���DISTINCT���ʽ�б�����Ϊ���������Ǵ������࣬�򷵻�1��������ݿ��а���һ
** ����֤��ѯ�����ȫ��ͬ��Ψһ�������ڣ������DISTINCT�б�������ġ�
*/
static int isDistinctRedundant(
  Parse *pParse,
  SrcList *pTabList,
  WhereClause *pWC,
  ExprList *pDistinct
){
  Table *pTab;
  Index *pIdx;
  int i;                          
  int iBase;

  /* If there is more than one table or sub-select in the FROM clause of
  ** this query, then it will not be possible to show that the DISTINCT 
<<<<<<< HEAD
  ** clause is redundant. */
  /* ����˴β�ѯ��FROM�Ӿ����ж�������Ӳ�ѯ���򲻻���ʾDISTINCT�Ӿ���
  ** ����ġ�
  */
=======
  ** clause is redundant.
  ** ����������ѯ��FORM�Ӿ����ж���һ�����sub-select
  ** ��ô����������ָʾDISTINCT�Ӿ��Ƕ���� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  if( pTabList->nSrc!=1 ) return 0;
  iBase = pTabList->a[0].iCursor;
  pTab = pTabList->a[0].pTab;

  /* If any of the expressions is an IPK column on table iBase, then return 
  ** true. Note: The (p->iTable==iBase) part of this test may be false if the
  ** current SELECT is a correlated sub-query.
  ** ����κ�һ�����ʽ�ڱ�iBase����һ��IPK�У���ô�ͷ���true.
  ** ע��:�����ǰ��SELECT��һ�����໥��ϵ���Ӳ�ѯ����ô������ԵĲ���(p->iTable==iBase)�����Ǵ�ġ�
  */
  /* ������iBase����������ʽ��IPK�������򷵻��档ע�������ǰSELECT��һ��
  ** ����Ӳ�ѯ����˴β����У�p->iTable==iBase�����ֿ���Ϊ�١�
  */
  for(i=0; i<pDistinct->nExpr; i++){
    Expr *p = pDistinct->a[i].pExpr;
    if( p->op==TK_COLUMN && p->iTable==iBase && p->iColumn<0 ) return 1;
  }

  /* Loop through all indices on the table, checking each to see if it makes
  ** the DISTINCT qualifier redundant. It does so if:
  **
  **   1. The index is itself UNIQUE, and
  **
  **   2. All of the columns in the index are either part of the pDistinct
  **      list, or else the WHERE clause contains a term of the form "col=X",
  **      where X is a constant value. The collation sequences of the
  **      comparison and select-list expressions must match those of the index.
  **
  **   3. All of those index columns for which the WHERE clause does not
  **      contain a "col=X" term are subject to a NOT NULL constraint.
  **
  ** ѭ�������ڱ��е��������������ÿ�������鿴���Ƿ��ʹDISTINCT���Ƴ�Ϊ����ġ�
  ** �����������������ô�ͻ�ʹDISTINCT���Ƴ�Ϊ�����:
  ** 	1.����������UNIQUE,����
  **		2.�������е����е���Ҫô��pDistinct�б��һ���֣�Ҫô�ǰ�����ʽΪ"col=X"��term��WHERE�Ӿ�(����X��һ������)��
  **		  �ȽϹ�ϵ���������к�select-list���ʽ����Ҳ��Щ������ƥ��
  **		3.WHERE�Ӿ�����е���Щ�����в�������һ��NOT NULLԼ����"col=X"term
  */
  /* �������ϵ�����ָ��,���ÿ��ָ���Ƿ�ʹDISTINCT�޶������ࡣ���ҽ�����������
  ** ����:
  **
  ** 1�������������UNIQUEԼ��������
  **
  ** 2��������������е��л�����pDistinct�б��еĲ��֣�����WHERE�Ӿ��а�������
  ** ��col=X���������X�ǳ������ȽϺ�ѡ���б���������б��ʽ����ƥ����Щ����
  ** ��
  ** 
  ** 3�����в�������col=X�����WHERE�Ӿ�������ж�����NOT NULLԼ��������
  */
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    if( pIdx->onError==OE_None ) continue;
    for(i=0; i<pIdx->nColumn; i++){
      int iCol = pIdx->aiColumn[i];
      if( 0==findTerm(pWC, iBase, iCol, ~(Bitmask)0, WO_EQ, pIdx) ){
        int iIdxCol = findIndexCol(pParse, pDistinct, iBase, pIdx, i);
        if( iIdxCol<0 || pTab->aCol[pIdx->aiColumn[i]].notNull==0 ){
          break;
        }
      }
    }
    if( i==pIdx->nColumn ){
<<<<<<< HEAD
      /* This index implies that the DISTINCT qualifier is redundant. */
	  /* ���������ζ��DISTINCT�޶���������ġ�*/
=======
      /* This index implies that the DISTINCT qualifier is redundant. ���������ʾDISTINCT�Ƕ���� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      return 1;
    }
  }

  return 0;
}

/*
** This routine decides if pIdx can be used to satisfy the ORDER BY
** clause.  If it can, it returns 1.  If pIdx cannot satisfy the
** ORDER BY clause, this routine returns 0.
**
** ����������ORDER BY�Ӿ��Ƿ���ʹ��pIdx.��������򷵻�1����������򷵻�0��
**
** pOrderBy is an ORDER BY clause from a SELECT statement.  pTab is the
** left-most table in the FROM clause of that same SELECT statement and
** the table has a cursor number of "base".  pIdx is an index on pTab.
**
** pOrderBy��SELECT�����е�һ��ORDER BY�Ӿ�.
** pTab������ͬ��SELECT�����е�FROM�Ӿ�������ߵı������������һ���������α���--"base".
** pIdx����pTab�ϵ�һ������
**
** nEqCol is the number of columns of pIdx that are used as equality
** constraints.  Any of these columns may be missing from the ORDER BY
** clause and the match can still be a success.
**
** nEqCol�Ǳ����ڵ�ʽ���ʽ�е�pIdx������.�κε���Щ�ж�������ORDER BY�Ӿ�����ʧ����ƥ�����ɿ��Գɹ���
**
** All terms of the ORDER BY that match against the index must be either
** ASC or DESC.  (Terms of the ORDER BY clause past the end of a UNIQUE
** index do not need to satisfy this constraint.)  The *pbRev value is
** set to 1 if the ORDER BY clause is all DESC and it is set to 0 if
** the ORDER BY clause is all ASC.
**
** ORDER BY������termsƥ������������ASC or DESC.(ORDER BY�Ӿ��terms����UNIQUE����ĩβ֮����Ҫ�������Լ��)
** ���ORDER BY�Ӿ���DESC����*pbRevֵ����Ϊ1�������ASC����Ϊ0.
*/
/* ������̾�����p�����Ƿ�����ORDER BY���Ӿ䡣������㣬�򷵻�1���������
** �㣬�򷵻�0��
**
** pOrderBy��SELECT����е�һ��ORDER BY�Ӿ���ʽ��pTab��ͬһ��SELECT�����
** ��FROM�Ӿ�������һ����񣬲����������ڿ�����ָ�롣p������pTab�ϵ�
** һ��������
**
** nEqCol��������ʽԼ����p�������������֡���ЩORDER BY�Ӿ��е�������������
** ���ܶ�ʧ����ƥ����Ȼ���Գɹ���
**
** ����ORDER BY�Ӿ��в�ƥ���������������ASC����DESC�롣(ORDER BY�Ӿ��
** ��ȥ��Ψһ��������Ҫ�������Լ����)���ORDER BY�Ӿ�ȫ����DESC����*pbRev
** ��ֵ��Ϊ1�����ORDER BY�Ӿ�ȫ����ASC����*pbRev��ֵ��Ϊ0��
*/
static int isSortingIndex(
<<<<<<< HEAD
	Parse *pParse,          /* Parsing context *//*����������*/
	WhereMaskSet *pMaskSet, /* Mapping from table cursor numbers to bitmaps *//* �ӱ��ָ��ӳ�䵽λͼ*/
	Index *pIdx,            /* The index we are testing *//* ���ڲ��Ե�����*/
	int base,               /* Cursor number for the table to be sorted *//* �����ñ��ָ��*/
	ExprList *pOrderBy,     /* The ORDER BY clause *//*ORDER BY�Ӿ�*/
	int nEqCol,             /* Number of index columns with == constraints *//* ����==Լ��������������������*/
	int wsFlags,            /* Index usages flags *//*����ʹ�ñ��*/
	int *pbRev              /* Set to 1 if ORDER BY is DESC *//* ���ORDER BY�Ӿ�ȫ����DESC������ֵΪ1*/
){
	int i, j;                       /* Loop counters *//* ѭ��������*/
	int sortOrder = 0;              /* XOR of index and ORDER BY sort direction *//* �����еĻ������Լ�ORDER BY������*/
  int nTerm;                      /* Number of ORDER BY terms *//* ORDER BY�������*/
  struct ExprList_item *pTerm;    /* A term of the ORDER BY clause *//* ORDER BY�Ӿ��е�һ����*/
=======
  Parse *pParse,          /* Parsing context ���������� */
  WhereMaskSet *pMaskSet, /* Mapping from table cursor numbers to bitmaps ���α�����λͼ��ӳ�� */
  Index *pIdx,            /* The index we are testing ���ǲ��Ե����� */
  int base,               /* Cursor number for the table to be sorted ��Ҫ����ı��α��� */
  ExprList *pOrderBy,     /* The ORDER BY clause ORDER BY�Ӿ� */
  int nEqCol,             /* Number of index columns with == constraints ==Լ������������ */
  int wsFlags,            /* Index usages flags ����ʹ�ñ�־ */
  int *pbRev              /* Set to 1 if ORDER BY is DESC ���ORDER BY��DESC����Ϊ1 */
){
  int i, j;                       /* Loop counters ѭ�������� */
  int sortOrder = 0;              /* XOR of index and ORDER BY sort direction ������XOR��ORDER BY�������� */
  int nTerm;                      /* Number of ORDER BY terms ORDER BY terms�� */
  struct ExprList_item *pTerm;    /* A term of the ORDER BY clause ORDER BY�Ӿ��һ��term */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  sqlite3 *db = pParse->db;

  if( !pOrderBy ) return 0;
  if( wsFlags & WHERE_COLUMN_IN ) return 0;
  if( pIdx->bUnordered ) return 0;

  nTerm = pOrderBy->nExpr;
  assert( nTerm>0 );

  /* Argument pIdx must either point to a 'real' named index structure, 
  ** or an index structure allocated on the stack by bestBtreeIndex() to
<<<<<<< HEAD
  ** represent the rowid index that is part of every table.  */
  /* p���������������ָ��һ������ʵ�ġ����������ṹ������һ�����䵽
  ** bestBtreeIndexջ�������ṹ������ʾrowidָ����ÿ�����е�һ���֡�
  */
=======
  ** represent the rowid index that is part of every table.  
  ** ����pIdx����ָ��һ��'real'�������������ݽṹ
  ** �����ڶ�ջ����bestBtreeIndex()����һ���������ݽṹ���ڱ�ʾÿ�����rowid����������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  assert( pIdx->zName || (pIdx->nColumn==1 && pIdx->aiColumn[0]==-1) );

  /* Match terms of the ORDER BY clause against columns of
  ** the index.
  **
  ** ��ORDER BY�Ӿ�terms��ƥ���������
  **
  ** Note that indices have pIdx->nColumn regular columns plus
  ** one additional column containing the rowid.  The rowid column
  ** of the index is also allowed to match against the ORDER BY
  ** clause.
  ** ע��:������pIdx->nColumn�����м���һ������rowid�ĸ�����.
  ** ����rowid��Ҳ���ܹ���ORDER BY�Ӿ���ƥ��
  */
  /* ƥ���������е��в�ͬ��ORDER BY�Ӿ��е��
  **
  ** ע��ָ��pIdx - > nColumn�����м�һ��������а���rowid��rowid
  ** �е�����Ҳ����ƥ��ORDER BY�Ӿ䡣
  */
  for(i=j=0, pTerm=pOrderBy->a; j<nTerm && i<=pIdx->nColumn; i++){
<<<<<<< HEAD
	  Expr *pExpr;       /* The expression of the ORDER BY pTerm *//* ORDER BY�Ӿ��еı��ʽpTerm*/
	  CollSeq *pColl;    /* The collating sequence of pExpr *//* ��������pExpr*/
	  int termSortOrder; /* Sort order for this term *//*�Դ����������*/
	  int iColumn;       /* The i-th column of the index.  -1 for rowid *//* �����еĵ�i��*/
	  int iSortOrder;    /* 1 for DESC, 0 for ASC on the i-th index term *//* ��i���������У�DESC��Ϊ1��ASC��Ϊ0*/
	  const char *zColl; /* Name of the collating sequence for i-th index term *//* ��i�����������������е�����*/
=======
    Expr *pExpr;       /* The expression of the ORDER BY pTerm ORDER BY���ʽpTerm */
    CollSeq *pColl;    /* The collating sequence of pExpr pExpr���������� */
    int termSortOrder; /* Sort order for this term ���term��������� */
    int iColumn;       /* The i-th column of the index.  -1 for rowid ������i-th�� */
    int iSortOrder;    /* 1 for DESC, 0 for ASC on the i-th index term ��i-th����term�У�0��ʾASC��1��ʾDESC */
    const char *zColl; /* Name of the collating sequence for i-th index term i-th����term������������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

    pExpr = pTerm->pExpr;
    if( pExpr->op!=TK_COLUMN || pExpr->iTable!=base ){
      /* Can not use an index sort on anything that is not a column in the
<<<<<<< HEAD
      ** left-most table of the FROM clause */
	  /*  �������FROM�Ӿ�������߱���һ�������ܶ�������ʹ����������
	  */
=======
      ** left-most table of the FROM clause
      ** �����ڲ���FROM�Ӿ��е�����ߵı��һ����ʹ��һ���������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      break;
    }
    pColl = sqlite3ExprCollSeq(pParse, pExpr);
    if( !pColl ){
      pColl = db->pDfltColl;
    }
    if( pIdx->zName && i<pIdx->nColumn ){
      iColumn = pIdx->aiColumn[i];
      if( iColumn==pIdx->pTable->iPKey ){
        iColumn = -1;
      }
      iSortOrder = pIdx->aSortOrder[i];
      zColl = pIdx->azColl[i];
    }else{
      iColumn = -1;
      iSortOrder = 0;
      zColl = pColl->zName;
    }
    if( pExpr->iColumn!=iColumn || sqlite3StrICmp(pColl->zName, zColl) ){
<<<<<<< HEAD
      /* Term j of the ORDER BY clause does not match column i of the index */
      /* ORDER BY�Ӿ��е�j�ƥ�������е�i�С�
	  */
=======
      /* Term j of the ORDER BY clause does not match column i of the index ORDER BY�Ӿ��Term j��ƥ��������i */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      if( i<nEqCol ){
        /* If an index column that is constrained by == fails to match an
        ** ORDER BY term, that is OK.  Just ignore that column of the index
        ** ���һ����==Լ���������в�ƥ��һ��ORDER BYterm���ǿ��Եġ�ֻҪ�������������
        */
		/* �����==��Լ���������в���ƥ��ORDER BY���ô��Ҳ�ǿ��Եģ�ֻ��Ҫ
		** �������������
		*/
        continue;
      }else if( i==pIdx->nColumn ){
<<<<<<< HEAD
        /* Index column i is the rowid.  All other terms match. */
		/* ������i��rowid����������������ƥ�䡣
		*/
=======
        /* Index column i is the rowid.  All other terms match. ������i��rowid.��������termsƥ�� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        break;
      }else{
        /* If an index column fails to match and is not constrained by ==
        ** then the index cannot satisfy the ORDER BY constraint.
        ** ���һ��������δ��ƥ�䲢�Ҳ�����==Լ���ģ���ô��������������ORDER BYԼ��
        */
	    /* ���һ�������в�ƥ�䣬����û��==Լ�����������������������ORDER
		** BYԼ��������
		*/
        return 0;
      }
    }
    assert( pIdx->aSortOrder!=0 || iColumn==-1 );
    assert( pTerm->sortOrder==0 || pTerm->sortOrder==1 );
    assert( iSortOrder==0 || iSortOrder==1 );
    termSortOrder = iSortOrder ^ pTerm->sortOrder;
    if( i>nEqCol ){
      if( termSortOrder!=sortOrder ){
        /* Indices can only be used if all ORDER BY terms past the ����ֻ�������е�ORDER BYterms��ȥ�ĵ�ʽԼ����DESC��ASC�ǲű�ʹ��
        ** equality constraints are all either DESC or ASC. */
		  /* ֻ��ORDER BY�Ӿ������ʽԼ�����������DESC�����ASC��
		  ** �ſ���ʹ��ָ����
		  */
        return 0;
      }
    }else{
      sortOrder = termSortOrder;
    }
    j++;
    pTerm++;
    if( iColumn<0 && !referencesOtherTables(pOrderBy, pMaskSet, j, base) ){
      /* If the indexed column is the primary key and everything matches
      ** so far and none of the ORDER BY terms to the right reference other
      ** tables in the join, then we are assured that the index can be used 
      ** to sort because the primary key is unique and so none of the other
      ** columns will make any difference
      **
      ** ������������������ҵ�ĿǰΪֹ��ƥ�䲢��û��ORDER BY terms�����ӵ�����������أ�
      ** ��ô���Ǳ�֤�������Ա�������ʹ�ã���Ϊ������Ψһ��������������û���κ�Ӱ��
      */
	  /* ���������������������ĿǰΪֹ�����ƥ�䣬����ORDER BY���ұߵ���
	  ** ��������ָ�������ı���ô���Ա�֤�������Ա�����������Ϊ������Ψ
	  ** һ�Ķ���û���������л������á�
	  */
      j = nTerm;
    }
  }

  *pbRev = sortOrder!=0;
  if( j>=nTerm ){
    /* All terms of the ORDER BY clause are covered by this index so ORDER BY�Ӿ������terms������������ǣ������������������������
    ** this index can be used for sorting. */
	  /* ����ORDER BY�Ӿ��е�����������������������������������
	  ** ��������
	  */
    return 1;
  }
  if( pIdx->onError!=OE_None && i==pIdx->nColumn
      && (wsFlags & WHERE_COLUMN_NULL)==0
      && !referencesOtherTables(pOrderBy, pMaskSet, j, base) 
  ){
    Column *aCol = pIdx->pTable->aCol;

    /* All terms of this index match some prefix of the ORDER BY clause,
    ** the index is UNIQUE, and no terms on the tail of the ORDER BY
    ** refer to other tables in a join. So, assuming that the index entries
    ** visited contain no NULL values, then this index delivers rows in
    ** the required order.
    **
    ** �������������terms��һЩORDER BY�Ӿ�ǰ׺��ƥ�䣬������UNIQUE,
    ** ������ORDER BYβ��û��terms�������е���������ء�
    ** ���ԣ��ٶ���������ʰ�����NULLֵ����ô��������ṩ�������˳��
    **
    ** It is not possible for any of the first nEqCol index fields to be
    ** NULL (since the corresponding "=" operator in the WHERE clause would 
    ** not be true). So if all remaining index columns have NOT NULL 
    ** constaints attached to them, we can be confident that the visited
<<<<<<< HEAD
    ** index entries are free of NULLs.  */
	/* ��������е����������ORDER BY�Ӿ��һЩǰ׺��������Ψһ�ġ�û��
	** ORDER BY�Ӿ���β������ָ�������ı����ԣ�����������Ŀ���ʲ�����NULL
	** ֵ,��ô��������ṩ�������˳��
	*/
=======
    ** index entries are free of NULLs.  
    **
    ** �κεĵ�һ��nEqCol�����ֶ�ΪNULLֵ�ǲ����ܵ�(��Ϊ��WHERE�Ӿ�����Ӧ"="�����������ȷ)��
    ** ���ԣ�����������µ���������NOT NULLԼ�������ǿ���ȷ����������û��NULLֵ��
    */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    for(i=nEqCol; i<pIdx->nColumn; i++){
      if( aCol[pIdx->aiColumn[i]].notNull==0 ) break;
    }
    return (i==pIdx->nColumn);
  }
  return 0;
}

/*
** Prepare a crude estimate of the logarithm of the input value.
** The results need not be exact.  This is only used for estimating
** the total cost of performing operations with O(logN) or O(NlogN)
** complexity.  Because N is just a guess, it is no great tragedy if
** logN is a little off.
**
** �ֲڵĹ���һ������ֵ�Ķ������������Ҫȷ�еء�
** ��ֻ����������ִ�и��Ӷ�ΪO(logN)��O(NlogN)�Ĳ������ܳɱ���
*/
/* ׼��һ������ֵ�Ķ����Ĵ��Թ��ơ��������Ҫ�ܾ�ȷ������������ڹ�
** ��ִ�в������ܴ�����O(logN)��O(NlogN)�����ԡ���ΪN������һ���²�ֵ
** ��ʹlogN��Щ���Ҳ���ⲻ��
*/
static double estLog(double N){
  double logN = 1;
  double x = 10;
  while( N>x ){
    logN += 1;
    x *= 10;
  }
  return logN;
}

/*
** Two routines for printing the content of an sqlite3_index_info
** structure.  Used for testing and debugging only.  If neither
** SQLITE_TEST or SQLITE_DEBUG are defined, then these routines
** are no-ops.
**
** �����������һ��sqlite3_index_info���ݽṹ�ĳ���
** ֻ�����ڲ��Ժ͵��ԡ����SQLITE_TEST��SQLITE_DEBUG��û���壬��ô��Щ������no-ops(�޲���)��
**
*/
/* sqlite3������Ϣ�ṹ�����ڴ�ӡĿ¼���������̡�ֻ���ڲ��Ժ͵��ԡ�
** ���SQLITE_TEST��SQLITE_DEBUG���������ˣ�������������ִ�пղ�����
*/
#if !defined(SQLITE_OMIT_VIRTUALTABLE) && defined(SQLITE_DEBUG)
static void TRACE_IDX_INPUTS(sqlite3_index_info *p){
  int i;
  if( !sqlite3WhereTrace ) return;
  for(i=0; i<p->nConstraint; i++){
    sqlite3DebugPrintf("  constraint[%d]: col=%d termid=%d op=%d usabled=%d\n",
       i,
       p->aConstraint[i].iColumn,
       p->aConstraint[i].iTermOffset,
       p->aConstraint[i].op,
       p->aConstraint[i].usable);
  }
  for(i=0; i<p->nOrderBy; i++){
    sqlite3DebugPrintf("  orderby[%d]: col=%d desc=%d\n",
       i,
       p->aOrderBy[i].iColumn,
       p->aOrderBy[i].desc);
  }
}
static void TRACE_IDX_OUTPUTS(sqlite3_index_info *p){
  int i;
  if( !sqlite3WhereTrace ) return;
  for(i=0; i<p->nConstraint; i++){
    sqlite3DebugPrintf("  usage[%d]: argvIdx=%d omit=%d\n",
       i,
       p->aConstraintUsage[i].argvIndex,
       p->aConstraintUsage[i].omit);
  }
  sqlite3DebugPrintf("  idxNum=%d\n", p->idxNum);
  sqlite3DebugPrintf("  idxStr=%s\n", p->idxStr);
  sqlite3DebugPrintf("  orderByConsumed=%d\n", p->orderByConsumed);
  sqlite3DebugPrintf("  estimatedCost=%g\n", p->estimatedCost);
}
#else
#define TRACE_IDX_INPUTS(A)
#define TRACE_IDX_OUTPUTS(A)
#endif

/* 
** Required because bestIndex() is called by bestOrClauseIndex() 
** ��ΪbestIndex()��bestOrClauseIndex()���ö�������
*/
/* ��������Ҫ�ģ���ΪbstIndex()��bestOrClauseIndex()���á�
*/
static void bestIndex(
    Parse*, WhereClause*, struct SrcList_item*,
    Bitmask, Bitmask, ExprList*, WhereCost*);

/*
** This routine attempts to find an scanning strategy that can be used 
** to optimize an 'OR' expression that is part of a WHERE clause. 
**
** ���������ͼ����һ���������Ż�һ��WHERE�Ӿ��е�һ��'OR'���ʽ��ɨ�����
**
** The table associated with FROM clause term pSrc may be either a
** regular B-Tree table or a virtual table.
**
** ��FROM�Ӿ�term pSrc�йصı������һ�������B-Tree���һ�������
*/
/* �����������ҵ�һ��ɨ����ԣ�������Կ��������Ż�һ��WHERE�Ӿ��е�
** ���򡱱��ʽ��
**
** ���Ӿ�FROM�е�pScr����صı�����Ǹ���ͨ��B���������һ�����
*/
static void bestOrClauseIndex(
<<<<<<< HEAD
	Parse *pParse,              /* The parsing context *//* �����ĵ�*/
	WhereClause *pWC,           /* The WHERE clause *//* WHERE�Ӿ�*/
struct SrcList_item *pSrc,  /* The FROM clause term to search *//* ����������FROM�Ӿ���*/
	Bitmask notReady,           /* Mask of cursors not available for indexing *//* ���������������α�����*/
	Bitmask notValid,           /* Cursors not available for any purpose *//* �������κ�Ŀ�ĵĹ��*/
  ExprList *pOrderBy,         /* The ORDER BY clause *//* ORDER BY�Ӿ�*/
  WhereCost *pCost            /* Lowest cost query plan *//* ��С����ѯ�ƻ��Ĵ���*/
){
#ifndef SQLITE_OMIT_OR_OPTIMIZATION
	const int iCur = pSrc->iCursor;   /* The cursor of the table to be accessed *//* ��ʹ�õı����*/
	const Bitmask maskSrc = getMask(pWC->pMaskSet, iCur);  /* Bitmask for pSrc *//* pSrc��λ����*/
	WhereTerm * const pWCEnd = &pWC->a[pWC->nTerm];        /* End of pWC->a[] *//* pWC->a[]����*/
	WhereTerm *pTerm;                 /* A single term of the WHERE clause *//* WHERE�Ӿ��һ����һ��*/

  /* The OR-clause optimization is disallowed if the INDEXED BY or
  ** NOT INDEXED clauses are used or if the WHERE_AND_ONLY bit is set. */
  /* ���INDEXED BY�Ӿ����NOT INDEXED�Ӿ��Ѿ�������WHERE_AND_ONLYλ����ô
  ** OR�Ӿ���Ż���Ч��
=======
  Parse *pParse,              /* The parsing context ���������� */
  WhereClause *pWC,           /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,  /* The FROM clause term to search Ҫ������FROM�Ӿ�term */
  Bitmask notReady,           /* Mask of cursors not available for indexing �α���������������Ч */
  Bitmask notValid,           /* Cursors not available for any purpose �α����κ���;�¶���Ч */
  ExprList *pOrderBy,         /* The ORDER BY clause ORDER BY�Ӿ� */
  WhereCost *pCost            /* Lowest cost query plan ��С���۵Ĳ�ѯ�ƻ� */
){
#ifndef SQLITE_OMIT_OR_OPTIMIZATION
  const int iCur = pSrc->iCursor;   /* The cursor of the table to be accessed ��Ҫ��ȡ�ı��α� */
  const Bitmask maskSrc = getMask(pWC->pMaskSet, iCur);  /* Bitmask for pSrc pSrc��λ���� */
  WhereTerm * const pWCEnd = &pWC->a[pWC->nTerm];        /* End of pWC->a[] pWC->a[]��ĩβ */
  WhereTerm *pTerm;                 /* A single term of the WHERE clause WHERE�Ӿ��һ������term */

  /* The OR-clause optimization is disallowed if the INDEXED BY or
  ** NOT INDEXED clauses are used or if the WHERE_AND_ONLY bit is set.
  ** ���ʹ��INDEXED BY��NOT INDEXED�Ӿ��������WHERE_AND_ONLY bit����ôOR�Ӿ��ǲ������Ż���
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pSrc->notIndexed || pSrc->pIndex!=0 ){
    return;
  }
  if( pWC->wctrlFlags & WHERE_AND_ONLY ){
    return;
  }

<<<<<<< HEAD
  /* Search the WHERE clause terms for a usable WO_OR term. *//* Ϊһ�������õ�WO_OR������WHERE�Ӿ���*/
=======
  /* Search the WHERE clause terms for a usable WO_OR term. ����WHERE�Ӿ�terms */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  for(pTerm=pWC->a; pTerm<pWCEnd; pTerm++){
    if( pTerm->eOperator==WO_OR 
     && ((pTerm->prereqAll & ~maskSrc) & notReady)==0
     && (pTerm->u.pOrInfo->indexable & maskSrc)!=0 
    ){
      WhereClause * const pOrWC = &pTerm->u.pOrInfo->wc;
      WhereTerm * const pOrWCEnd = &pOrWC->a[pOrWC->nTerm];
      WhereTerm *pOrTerm;
      int flags = WHERE_MULTI_OR;
      double rTotal = 0;
      double nRow = 0;
      Bitmask used = 0;

      for(pOrTerm=pOrWC->a; pOrTerm<pOrWCEnd; pOrTerm++){
        WhereCost sTermCost;
        WHERETRACE(("... Multi-index OR testing for term %d of %d....\n", 
          (pOrTerm - pOrWC->a), (pTerm - pWC->a)
        ));
        if( pOrTerm->eOperator==WO_AND ){
          WhereClause *pAndWC = &pOrTerm->u.pAndInfo->wc;
          bestIndex(pParse, pAndWC, pSrc, notReady, notValid, 0, &sTermCost);
        }else if( pOrTerm->leftCursor==iCur ){
          WhereClause tempWC;
          tempWC.pParse = pWC->pParse;
          tempWC.pMaskSet = pWC->pMaskSet;
          tempWC.pOuter = pWC;
          tempWC.op = TK_AND;
          tempWC.a = pOrTerm;
          tempWC.wctrlFlags = 0;
          tempWC.nTerm = 1;
          bestIndex(pParse, &tempWC, pSrc, notReady, notValid, 0, &sTermCost);
        }else{
          continue;
        }
        rTotal += sTermCost.rCost;
        nRow += sTermCost.plan.nRow;
        used |= sTermCost.used;
        if( rTotal>=pCost->rCost ) break;
      }

      /* If there is an ORDER BY clause, increase the scan cost to account 
      ** for the cost of the sort. */
	  /* �������һ��ORDER BY�Ӿ䣬��Ϊ�˴�����Ĵ����˻�����һ��������ۡ�
	  */
      if( pOrderBy!=0 ){
        WHERETRACE(("... sorting increases OR cost %.9g to %.9g\n",
                    rTotal, rTotal+nRow*estLog(nRow)));
        rTotal += nRow*estLog(nRow);
      }

      /* If the cost of scanning using this OR term for optimization is
      ** less than the current cost stored in pCost, replace the contents
<<<<<<< HEAD
      ** of pCost. */
	  /* ��������Ż���ʹ�ô˴�OR��������������pCost�е�ǰ��������ۣ�
	  ** ���滻pCost�е����ݡ�
	  */
=======
      ** of pCost. 
      ** ���ʹ���Ż���ORterm��ɨ����۱ȴ洢��pCost�ĵ�ǰ���۸��٣��滻pCost������
      */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      WHERETRACE(("... multi-index OR cost=%.9g nrow=%.9g\n", rTotal, nRow));
      if( rTotal<pCost->rCost ){
        pCost->rCost = rTotal;
        pCost->used = used;
        pCost->plan.nRow = nRow;
        pCost->plan.wsFlags = flags;
        pCost->plan.u.pTerm = pTerm;
      }
    }
  }
#endif /* SQLITE_OMIT_OR_OPTIMIZATION *//* SQLITE_OMIT_OR_OPTIMIZATION����*/
}

#ifndef SQLITE_OMIT_AUTOMATIC_INDEX
/*
** Return TRUE if the WHERE clause term pTerm is of a form where it
** could be used with an index to access pSrc, assuming an appropriate
** index existed.
**
** ���WHERE�Ӿ�term pTerm������һ����ʽ��������һ������һ��ʹ��������pSrc���������һ���ʵ����������򷵻�TRUE
**
*/
/* ���WHERE�Ӿ���pTerm�ǿ������ں�һ��������ͬ���ӵ�pSrc��һ����ʽ��
** �����棬����һ�����ʵ������Ǵ��ڵġ�
*/
static int termCanDriveIndex(
<<<<<<< HEAD
	WhereTerm *pTerm,              /* WHERE clause term to check *//* ���ڼ���WHERE�Ӿ���*/
struct SrcList_item *pSrc,     /* Table we are trying to access *//* ���Ե�½�ı��*/
	Bitmask notReady               /* Tables in outer loops of the join *//* ���ӵ��ⲿѭ�����*/
=======
  WhereTerm *pTerm,              /* WHERE clause term to check ��Ҫ����WHERE�Ӿ� */
  struct SrcList_item *pSrc,     /* Table we are trying to access ������ͼ���ʵı� */
  Bitmask notReady               /* Tables in outer loops of the join �����ӵ��ⲿѭ���ı� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  char aff;
  if( pTerm->leftCursor!=pSrc->iCursor ) return 0;
  if( pTerm->eOperator!=WO_EQ ) return 0;
  if( (pTerm->prereqRight & notReady)!=0 ) return 0;
  aff = pSrc->pTab->aCol[pTerm->u.leftColumn].affinity;
  if( !sqlite3IndexAffinityOk(pTerm->pExpr, aff) ) return 0;
  return 1;
}
#endif

#ifndef SQLITE_OMIT_AUTOMATIC_INDEX
/*
** If the query plan for pSrc specified in pCost is a full table scan
** and indexing is allows (if there is no NOT INDEXED clause) and it
** possible to construct a transient index that would perform better
** than a full table scan even when the cost of constructing the index
** is taken into account, then alter the query plan to use the
** transient index.
**
** �����pCost��ָ����pSrc�Ĳ�ѯ�ƻ���һ��ȫ��ɨ�貢�ҿ���ʹ������(���û��INDEXED�Ӿ�)
** ���������ܴ���һ����ʱ����������ʹ�����������Ĵ���Ҳ�����ǽ�ȥ���ɱ�ȫ��ɨ����ã�
** ��ô�ı��ѯ�ƻ���ʹ����ʱ������
*/
/* ���pCost��pSrc��ϸ˵���Ĳ�ѯ�ƻ���һ�������ѯ���������������
** �����û��NOT INDEXED�Ӿ���ڣ����ҿ��Թ���һ��˲ָ̬��,��ִ�б�ȫ
** ��ɨ�輴ʹ���������Ĵ��ۿ��ǽ�ȥ,Ȼ��ı��ѯ�ƻ�ʹ��˲ָ̬����
*/
static void bestAutomaticIndex(
<<<<<<< HEAD
	Parse *pParse,              /* The parsing context *//* �����ĵ�*/
	WhereClause *pWC,           /* The WHERE clause *//* WHERE�Ӿ�*/
struct SrcList_item *pSrc,  /* The FROM clause term to search *//* ����������FROM�Ӿ���*/
	Bitmask notReady,           /* Mask of cursors that are not available *//* ���������������α�����*/
	WhereCost *pCost            /* Lowest cost query plan *//* ��С����ѯ�ƻ��Ĵ���*/
){
	double nTableRow;           /* Rows in the input table *//* ��������*/
	double logN;                /* log(nTableRow) */
	double costTempIdx;         /* per-query cost of the transient index *//* ˲ָ̬����ÿ����ѯ����*/
	WhereTerm *pTerm;           /* A single term of the WHERE clause *//* һ��WHERE�Ӿ�ĵ�һ��*/
	WhereTerm *pWCEnd;          /* End of pWC->a[] *//*pWC->a[]����*/
	Table *pTable;              /* Table tht might be indexed *//* tht����ܱ�������*/

  if( pParse->nQueryLoop<=(double)1 ){
    /* There is no point in building an automatic index for a single scan */
    /* Ϊ����ɨ�轨��һ���Զ�������û������ġ�
    */
    return;
  }
  if( (pParse->db->flags & SQLITE_AutoIndex)==0 ){
    /* Automatic indices are disabled at run-time */
	  /* ������ʱ�Զ�ָ���ǽ��õġ�
	  */
    return;
  }
  if( (pCost->plan.wsFlags & WHERE_NOT_FULLSCAN)!=0 ){
    /* We already have some kind of index in use for this query. */
	  /* �����ѯ�������Ѿ�����һЩ��ʹ���е�������
	  */
    return;
  }
  if( pSrc->notIndexed ){
    /* The NOT INDEXED clause appears in the SQL. */
	  /* SQL�г��ֵ�NOT INDEXED�Ӿ䡣
	  */
    return;
  }
  if( pSrc->isCorrelated ){
    /* The source is a correlated sub-query. No point in indexing it. */
	  /* ���Դ��һ������Ӳ�ѯ������������û������ġ�
	  */
=======
  Parse *pParse,              /* The parsing context ���������� */
  WhereClause *pWC,           /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,  /* The FROM clause term to search ���ڲ�ѯ��FROM�Ӿ�term */
  Bitmask notReady,           /* Mask of cursors that are not available �α����������Ч�� */
  WhereCost *pCost            /* Lowest cost query plan ��С���۲�ѯ�ƻ� */
){
  double nTableRow;           /* Rows in the input table ��������е��� */
  double logN;                /* log(nTableRow) */
  double costTempIdx;         /* per-query cost of the transient index ��ʱ������per-query���� */
  WhereTerm *pTerm;           /* A single term of the WHERE clause WHERE�Ӿ��һ������term */
  WhereTerm *pWCEnd;          /* End of pWC->a[] pWC->a[]��ĩβ */
  Table *pTable;              /* Table tht might be indexed �����������ı�tht */

  if( pParse->nQueryLoop<=(double)1 ){
    /* There is no point in building an automatic index for a single scan Ϊһ����һ��ɨ�蹹��һ���Զ������ǲ���Ҫ�� */
    return;
  }
  if( (pParse->db->flags & SQLITE_AutoIndex)==0 ){
    /* Automatic indices are disabled at run-time ������ʱ�����Զ���������Ч�� */
    return;
  }
  if( (pCost->plan.wsFlags & WHERE_NOT_FULLSCAN)!=0 ){
    /* We already have some kind of index in use for this query. ��ʹ�������ѯʱ�Ѿ��в�������� */
    return;
  }
  if( pSrc->notIndexed ){
    /* The NOT INDEXED clause appears in the SQL. ��SQL�г���NOT INDEXED�Ӿ� */
    return;
  }
  if( pSrc->isCorrelated ){
    /* The source is a correlated sub-query. No point in indexing it. ��Դ��һ���й������Ӳ�ѯ������Ҫʹ������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    return;
  }

  assert( pParse->nQueryLoop >= (double)1 );
  pTable = pSrc->pTab;
  nTableRow = pTable->nRowEst;
  logN = estLog(nTableRow); //����ִ�и��Ӷ�
  costTempIdx = 2*logN*(nTableRow/pParse->nQueryLoop + 1); //��ʱ�����Ĵ���
  if( costTempIdx>=pCost->rCost ){//������ʱ��Ĵ��۴���ȫ��ɨ��Ĵ���
    /* The cost of creating the transient table would be greater than ������ʱ��Ĵ��۴���ȫ��ɨ��Ĵ���
    ** doing the full table scan */
	  /* ������ʱ��Ĵ��ۻ��ȫ��ɨ��ߡ�
	  */
    return;
  }

<<<<<<< HEAD
  /* Search for any equality comparison term */
  /* ��������ĵ�ֵ�Ƚ��
  */
=======
  /* Search for any equality comparison term �����κε�ʽ�Ƚϵ�term */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  pWCEnd = &pWC->a[pWC->nTerm];
  for(pTerm=pWC->a; pTerm<pWCEnd; pTerm++){ //ѭ������where�Ӿ��е�ÿ��term
    if( termCanDriveIndex(pTerm, pSrc, notReady) ){	//���term����ʹ������
      WHERETRACE(("auto-index reduces cost from %.1f to %.1f\n",
                    pCost->rCost, costTempIdx));
      pCost->rCost = costTempIdx;
      pCost->plan.nRow = logN + 1;
      pCost->plan.wsFlags = WHERE_TEMP_INDEX;
      pCost->used = pTerm->prereqRight;
      break;
    }
  }
}
#else
# define bestAutomaticIndex(A,B,C,D,E)  /* no-op *//* �ղ���*/
#endif /* SQLITE_OMIT_AUTOMATIC_INDEX *//* SQLITE_OMIT_AUTOMATIC_INDEX������*/


#ifndef SQLITE_OMIT_AUTOMATIC_INDEX
/*
** Generate code to construct the Index object for an automatic index
** and to set up the WhereLevel object pLevel so that the code generator
** makes use of the automatic index.
**
** ���ɴ����������������������Զ�����������������WhereLevel����pLevel�Ա����������ʹ���Զ�����
*/
/* Ϊ�����������ɴ��빹���Զ���������������WhereLevel����pLevel��������
** �����������������Զ�������
*/
static void constructAutomaticIndex(
<<<<<<< HEAD
	Parse *pParse,              /* The parsing context *//* �����ĵ�*/
	WhereClause *pWC,           /* The WHERE clause *//* WHERE�Ӿ�*/
struct SrcList_item *pSrc,  /* The FROM clause term to get the next index *//* ����������һ��������FROM�Ӿ���*/
  Bitmask notReady,           /* Mask of cursors that are not available *//* ���������������α�����*/
  WhereLevel *pLevel          /* Write new index here *//* �ڴ�д������*/
){
	int nColumn;                /* Number of columns in the constructed index *//* ��������������*/
	WhereTerm *pTerm;           /* A single term of the WHERE clause *//* WHERE�Ӿ��һ����һ��*/
	WhereTerm *pWCEnd;          /* End of pWC->a[] *//*pWC->a[]����*/
	int nByte;                  /* Byte of memory needed for pIdx *//* pIdx������ڴ��С*/
	Index *pIdx;                /* Object describing the transient index *//* ����˲̬������Ŀ��*/
	Vdbe *v;                    /* Prepared statement under construction *//* ׼���ô��ڽ������е�����*/
	int addrInit;               /* Address of the initialization bypass jump *//* ������ʼ���ĵ�ַ*/
	Table *pTable;              /* The table being indexed *//* ���������ı�*/
	KeyInfo *pKeyinfo;          /* Key information for the index */   /* �����еĹؼ���Ϣ*/
	int addrTop;                /* Top of the index fill loop *//* �������ѭ���Ķ���*/
	int regRecord;              /* Register holding an index record *//* ע�ᱣ��һ��������¼*/
	int n;                      /* Column counter *//* ����������*/
	int i;                      /* Loop counter *//* ѭ��������*/
	int mxBitCol;               /* Maximum column in pSrc->colUsed *//* pSrc-��colUsed���������*/
	CollSeq *pColl;             /* Collating sequence to on a column *//* ��������*/
	Bitmask idxCols;            /* Bitmap of columns used for indexing *//* �����������е�λͼ*/
	Bitmask extraCols;          /* Bitmap of additional columns *//* ����е�λͼ*/

  /* Generate code to skip over the creation and initialization of the
  ** transient index on 2nd and subsequent iterations of the loop. */
	/* ���ɴ���������ѭ���ڶ�����֮��ĵ�����˲̬�����Ĵ����ͳ�ʼ����
	*/
=======
  Parse *pParse,              /* The parsing context ���������� */
  WhereClause *pWC,           /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,  /* The FROM clause term to get the next index FROM�Ӿ�termΪ�˵õ���һ������ */
  Bitmask notReady,           /* Mask of cursors that are not available �α����������Ч�� */
  WhereLevel *pLevel          /* Write new index here д���µ����� */
){
  int nColumn;                /* Number of columns in the constructed index �ڹ���������е����� */
  WhereTerm *pTerm;           /* A single term of the WHERE clause WHERE�Ӿ��һ����һ��term */
  WhereTerm *pWCEnd;          /* End of pWC->a[] pWC->a[]��ĩβ */
  int nByte;                  /* Byte of memory needed for pIdx pIdx��Ҫ���ڴ��ֽ� */
  Index *pIdx;                /* Object describing the transient index ������ʱ�����Ķ��� */
  Vdbe *v;                    /* Prepared statement under construction �ڽ�����׼���õ����� */
  int addrInit;               /* Address of the initialization bypass jump ��ʼ����ַ�������� */
  Table *pTable;              /* The table being indexed �������ı� */
  KeyInfo *pKeyinfo;          /* Key information for the index �����Ĺؼ���Ϣ */   
  int addrTop;                /* Top of the index fill loop ���ѭ������������ */
  int regRecord;              /* Register holding an index record ��¼����һ��������¼ */
  int n;                      /* Column counter �м����� */
  int i;                      /* Loop counter ѭ�������� */
  int mxBitCol;               /* Maximum column in pSrc->colUsed ��pSrc->colUsed�е������� */
  CollSeq *pColl;             /* Collating sequence to on a column ��һ�����е��������� */
  Bitmask idxCols;            /* Bitmap of columns used for indexing �����������е�λ���� */
  Bitmask extraCols;          /* Bitmap of additional columns �����е�λ���� */

  /* Generate code to skip over the creation and initialization of the
  ** transient index on 2nd and subsequent iterations of the loop. 
  ** ���ɴ�������������ѭ����2nd����������ʱ����ʱ�����Ĵ����ͳ�ʼ��
  */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  v = pParse->pVdbe;
  assert( v!=0 );
  addrInit = sqlite3CodeOnce(pParse);

  /* Count the number of columns that will be added to the index ���㽫Ҫ��ӵ�����������������ƥ��WHERE�Ӿ��Լ��
  ** and used to match WHERE clause constraints */
  /* �Խ�Ҫ������������������WHERE�Ӿ�����������������
  */
  nColumn = 0;
  pTable = pSrc->pTab;
  pWCEnd = &pWC->a[pWC->nTerm];
  idxCols = 0;
  for(pTerm=pWC->a; pTerm<pWCEnd; pTerm++){
    if( termCanDriveIndex(pTerm, pSrc, notReady) ){
      int iCol = pTerm->u.leftColumn;
      Bitmask cMask = iCol>=BMS ? ((Bitmask)1)<<(BMS-1) : ((Bitmask)1)<<iCol;
      testcase( iCol==BMS );
      testcase( iCol==BMS-1 );
      if( (idxCols & cMask)==0 ){
        nColumn++;
        idxCols |= cMask;
      }
    }
  }
  assert( nColumn>0 );
  pLevel->plan.nEq = nColumn;

  /* Count the number of additional columns needed to create a
  ** covering index.  A "covering index" is an index that contains all
  ** columns that are needed by the query.  With a covering index, the
  ** original table never needs to be accessed.  Automatic indices must
  ** be a covering index because the index will not be updated if the
  ** original table changes and the index and table cannot both be used
  ** if they go out of sync.
  **
  ** ������Ҫ����һ�����������ĸ����е���ֵ��һ��"��������"��һ���������б���ѯ���е�������
  ** ����һ������������ԭʼ������Ҫ�ٱ����ʡ�
  ** ��Ϊ���ԭʼ��仯,������������£��Զ�����������һ������������������������ͱ�ͬ���Ļ������Ƕ������ᱻʹ�á�
  **
  */
  /* ���������е�������Ҫ����һ��������������������������ָһ��������
  ** �в�ѯ������е����������˸�����������ôԭʼ��Ͳ���Ҫ�����ʡ��Զ�
  ** ����������һ��������������Ϊ������Ǳ�����ͬ���Ļ���ԭʼ���ͻ��
  ** ������Ҳͬʱ���ܱ�ʹ�ã���ô��������Ͳ��ܱ����¡�
  */
  extraCols = pSrc->colUsed & (~idxCols | (((Bitmask)1)<<(BMS-1)));
  mxBitCol = (pTable->nCol >= BMS-1) ? BMS-1 : pTable->nCol;
  testcase( pTable->nCol==BMS-1 );
  testcase( pTable->nCol==BMS-2 );
  for(i=0; i<mxBitCol; i++){
    if( extraCols & (((Bitmask)1)<<i) ) nColumn++;
  }
  if( pSrc->colUsed & (((Bitmask)1)<<(BMS-1)) ){
    nColumn += pTable->nCol - BMS + 1;
  }
  pLevel->plan.wsFlags |= WHERE_COLUMN_EQ | WHERE_IDX_ONLY | WO_EQ;

<<<<<<< HEAD
  /* Construct the Index object to describe this index */
  /* ���������������������������
  */
=======
  /* Construct the Index object to describe this index ������������������������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  nByte = sizeof(Index);
  nByte += nColumn*sizeof(int);     /* Index.aiColumn */
  nByte += nColumn*sizeof(char*);   /* Index.azColl */
  nByte += nColumn;                 /* Index.aSortOrder */
  pIdx = sqlite3DbMallocZero(pParse->db, nByte);
  if( pIdx==0 ) return;
  pLevel->plan.u.pIdx = pIdx;
  pIdx->azColl = (char**)&pIdx[1];
  pIdx->aiColumn = (int*)&pIdx->azColl[nColumn];
  pIdx->aSortOrder = (u8*)&pIdx->aiColumn[nColumn];
  pIdx->zName = "auto-index";
  pIdx->nColumn = nColumn;
  pIdx->pTable = pTable;
  n = 0;
  idxCols = 0;
  for(pTerm=pWC->a; pTerm<pWCEnd; pTerm++){
    if( termCanDriveIndex(pTerm, pSrc, notReady) ){
      int iCol = pTerm->u.leftColumn;
      Bitmask cMask = iCol>=BMS ? ((Bitmask)1)<<(BMS-1) : ((Bitmask)1)<<iCol;
      if( (idxCols & cMask)==0 ){
        Expr *pX = pTerm->pExpr;
        idxCols |= cMask;
        pIdx->aiColumn[n] = pTerm->u.leftColumn;
        pColl = sqlite3BinaryCompareCollSeq(pParse, pX->pLeft, pX->pRight);
        pIdx->azColl[n] = ALWAYS(pColl) ? pColl->zName : "BINARY";
        n++;
      }
    }
  }
  assert( (u32)n==pLevel->plan.nEq );

  /* Add additional columns needed to make the automatic index into ��Ҫ��Ӹ�����ʹ�Զ�������Ϊ��������
  ** a covering index */
  /* ���һ�����⵫�����������֤�Զ�������һ������������
  */
  for(i=0; i<mxBitCol; i++){
    if( extraCols & (((Bitmask)1)<<i) ){
      pIdx->aiColumn[n] = i;
      pIdx->azColl[n] = "BINARY";
      n++;
    }
  }
  if( pSrc->colUsed & (((Bitmask)1)<<(BMS-1)) ){
    for(i=BMS-1; i<pTable->nCol; i++){
      pIdx->aiColumn[n] = i;
      pIdx->azColl[n] = "BINARY";
      n++;
    }
  }
  assert( n==nColumn );

<<<<<<< HEAD
  /* Create the automatic index */
  /* ����һ���Զ�����*/
=======
  /* Create the automatic index �����Զ����� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  pKeyinfo = sqlite3IndexKeyinfo(pParse, pIdx);
  assert( pLevel->iIdxCur>=0 );
  sqlite3VdbeAddOp4(v, OP_OpenAutoindex, pLevel->iIdxCur, nColumn+1, 0,
                    (char*)pKeyinfo, P4_KEYINFO_HANDOFF);
  VdbeComment((v, "for %s", pTable->zName));

<<<<<<< HEAD
  /* Fill the automatic index with content */
  /* �����Զ�����������*/
=======
  /* Fill the automatic index with content ����Զ����������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  addrTop = sqlite3VdbeAddOp1(v, OP_Rewind, pLevel->iTabCur);
  regRecord = sqlite3GetTempReg(pParse);
  sqlite3GenerateIndexKey(pParse, pIdx, pLevel->iTabCur, regRecord, 1);
  sqlite3VdbeAddOp2(v, OP_IdxInsert, pLevel->iIdxCur, regRecord);
  sqlite3VdbeChangeP5(v, OPFLAG_USESEEKRESULT);
  sqlite3VdbeAddOp2(v, OP_Next, pLevel->iTabCur, addrTop+1);
  sqlite3VdbeChangeP5(v, SQLITE_STMTSTATUS_AUTOINDEX);
  sqlite3VdbeJumpHere(v, addrTop);
  sqlite3ReleaseTempReg(pParse, regRecord);
  
<<<<<<< HEAD
  /* Jump here when skipping the initialization */
  /* ������ʼ��ʱ�������˴�*/
=======
  /* Jump here when skipping the initialization ��������ʼ��ʱ�������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  sqlite3VdbeJumpHere(v, addrInit);
}
#endif /* SQLITE_OMIT_AUTOMATIC_INDEX *//* SQLITE_OMIT_AUTOMATIC_INDEX����*/

#ifndef SQLITE_OMIT_VIRTUALTABLE
/*
** Allocate and populate an sqlite3_index_info structure. It is the 
** responsibility of the caller to eventually release the structure
** by passing the pointer returned by this function to sqlite3_free().
**
** ��������һ��sqlite3_index_info���ݽṹ��
** �����������õ�����ͨ������������ظ�sqlite3_free()��ָ�������ͷ����ݽṹ
**
*/
/* ��������һ��sqlite3_index_info�ṹ��ͨ��sqlite3_free()�������ص�
** ָ��������ͷ��������ĵ����߸���ġ�
*/
static sqlite3_index_info *allocateIndexInfo(
  Parse *pParse, 
  WhereClause *pWC,
  struct SrcList_item *pSrc,
  ExprList *pOrderBy
){
  int i, j;
  int nTerm;
  struct sqlite3_index_constraint *pIdxCons;
  struct sqlite3_index_orderby *pIdxOrderBy;
  struct sqlite3_index_constraint_usage *pUsage;
  WhereTerm *pTerm;
  int nOrderBy;
  sqlite3_index_info *pIdxInfo;

  WHERETRACE(("Recomputing index info for %s...\n", pSrc->pTab->zName));

  /* Count the number of possible WHERE clause constraints referring ͳ����������������������WHERE�Ӿ�ĸ���
  ** to this virtual table */
  /* ����ָ��������Ŀ��ܵ�WHERE�Ӿ�Լ����������
  */
  for(i=nTerm=0, pTerm=pWC->a; i<pWC->nTerm; i++, pTerm++){
    if( pTerm->leftCursor != pSrc->iCursor ) continue;
    assert( (pTerm->eOperator&(pTerm->eOperator-1))==0 );
    testcase( pTerm->eOperator==WO_IN );
    testcase( pTerm->eOperator==WO_ISNULL );
    if( pTerm->eOperator & (WO_IN|WO_ISNULL) ) continue;
    if( pTerm->wtFlags & TERM_VNULL ) continue;
    nTerm++;
  }

  /* If the ORDER BY clause contains only columns in the current 
  ** virtual table then allocate space for the aOrderBy part of
  ** the sqlite3_index_info structure.
  **
  ** ���ORDER BY�Ӿ�ֻ�����ڵ�ǰ�����У���ôΪsqlite3_index_info���ݽṹ��aOrderBy���ַ���ռ�
  */
  /* ���ORDER BY�Ӿ�ֻ������ǰ�����У���ôΪsqlite3_index_info
  ** �ṹ�е�aOrderby���ַ���ռ䡣
  */
  nOrderBy = 0;
  if( pOrderBy ){
    for(i=0; i<pOrderBy->nExpr; i++){
      Expr *pExpr = pOrderBy->a[i].pExpr;
      if( pExpr->op!=TK_COLUMN || pExpr->iTable!=pSrc->iCursor ) break;
    }
    if( i==pOrderBy->nExpr ){
      nOrderBy = pOrderBy->nExpr;
    }
  }

  /* Allocate the sqlite3_index_info structure ����sqlite3_index_info���ݽṹ
  */
  /* ����sqlite3_index_info�ṹ��
  */
  pIdxInfo = sqlite3DbMallocZero(pParse->db, sizeof(*pIdxInfo)
                           + (sizeof(*pIdxCons) + sizeof(*pUsage))*nTerm
                           + sizeof(*pIdxOrderBy)*nOrderBy );
  if( pIdxInfo==0 ){
    sqlite3ErrorMsg(pParse, "out of memory");
    /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
    return 0;
  }

  /* Initialize the structure.  The sqlite3_index_info structure contains
  ** many fields that are declared "const" to prevent xBestIndex from
  ** changing them.  We have to do some funky casting in order to
  ** initialize those fields.
  **
  ** ��ʼ�����ݽṹ��sqlite3_index_info���ݽṹ������౻����Ϊ"const"�Ӷ�������ֹxBestIndex���ı���ֶΡ�
  ** Ϊ�˳�ʼ����Щ�ֶΣ����ǲ��ò���һЩ�ر��ת����
  */
  /* ��ʼ���ṹ��sqlite3_index_info�ṹ��������ֶ�������������,�Է�ֹ
  ** xBestIndex�ı����ǡ����Ǳ�����һЩ��������ʼ����Щ�ֶΡ�
  */
  pIdxCons = (struct sqlite3_index_constraint*)&pIdxInfo[1];
  pIdxOrderBy = (struct sqlite3_index_orderby*)&pIdxCons[nTerm];
  pUsage = (struct sqlite3_index_constraint_usage*)&pIdxOrderBy[nOrderBy];
  *(int*)&pIdxInfo->nConstraint = nTerm;
  *(int*)&pIdxInfo->nOrderBy = nOrderBy;
  *(struct sqlite3_index_constraint**)&pIdxInfo->aConstraint = pIdxCons;
  *(struct sqlite3_index_orderby**)&pIdxInfo->aOrderBy = pIdxOrderBy;
  *(struct sqlite3_index_constraint_usage**)&pIdxInfo->aConstraintUsage =
                                                                   pUsage;

  for(i=j=0, pTerm=pWC->a; i<pWC->nTerm; i++, pTerm++){
    if( pTerm->leftCursor != pSrc->iCursor ) continue;
    assert( (pTerm->eOperator&(pTerm->eOperator-1))==0 );
    testcase( pTerm->eOperator==WO_IN );
    testcase( pTerm->eOperator==WO_ISNULL );
    if( pTerm->eOperator & (WO_IN|WO_ISNULL) ) continue;
    if( pTerm->wtFlags & TERM_VNULL ) continue;
    pIdxCons[j].iColumn = pTerm->u.leftColumn;
    pIdxCons[j].iTermOffset = i;
    pIdxCons[j].op = (u8)pTerm->eOperator;
    /* The direct assignment in the previous line is possible only because
    ** the WO_ and SQLITE_INDEX_CONSTRAINT_ codes are identical.  The
<<<<<<< HEAD
    ** following asserts verify this fact. */
	/* ��ǰһ��ֱ�Ӹ�ֵ����������WO_��SQLITE_INDEX_CONSTRAINT_���붼����ͬ
	** �ġ����¼���������֤����������
	*/
=======
    ** following asserts verify this fact. 
    **
    ** ֻ��ΪWO_��SQLITE_INDEX_CONSTRAINT_��������ȫ��ͬ�ģ�������ǰһ��ֱ�ӷ����ǿ��ܵġ�
    ** ����asserts��֤�����ʵ
    */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    assert( WO_EQ==SQLITE_INDEX_CONSTRAINT_EQ );
    assert( WO_LT==SQLITE_INDEX_CONSTRAINT_LT );
    assert( WO_LE==SQLITE_INDEX_CONSTRAINT_LE );
    assert( WO_GT==SQLITE_INDEX_CONSTRAINT_GT );
    assert( WO_GE==SQLITE_INDEX_CONSTRAINT_GE );
    assert( WO_MATCH==SQLITE_INDEX_CONSTRAINT_MATCH );
    assert( pTerm->eOperator & (WO_EQ|WO_LT|WO_LE|WO_GT|WO_GE|WO_MATCH) );
    j++;
  }
  for(i=0; i<nOrderBy; i++){
    Expr *pExpr = pOrderBy->a[i].pExpr;
    pIdxOrderBy[i].iColumn = pExpr->iColumn;
    pIdxOrderBy[i].desc = pOrderBy->a[i].sortOrder;
  }

  return pIdxInfo;
}

/*
** The table object reference passed as the second argument to this function
** must represent a virtual table. This function invokes the xBestIndex()
** method of the virtual table with the sqlite3_index_info pointer passed
** as the argument.
**
** ����������еĵڶ�������--��������ñ����ʾһ�������
** ��������������������sqlite3_index_infoָ���xBestIndex()������Ϊ����
**
** If an error occurs, pParse is populated with an error message and a
** non-zero value is returned. Otherwise, 0 is returned and the output
** part of the sqlite3_index_info structure is left populated.
**
** ���һ��������֣�pParse��һ��������Ϣ��䲢��һ����0ֵ���ᱻ���ء�
** ���򣬾ͷ���0�������ʣ�µ�sqlite3_index_info���ݽṹ��������֡�
**
** Whether or not an error is returned, it is the responsibility of the
** caller to eventually free p->idxStr if p->needToFreeIdxStr indicates
** that this is required.
**
** �����Ƿ񷵻�һ��������Ϣ�����p->needToFreeIdxStr�����Ǳ���ģ���ô�����ջ��û��������ͷ�p->idxStr
*/
/* �����������Ϊ�ڶ����������ݸ��ú����������һ����������������
** xBestIndex()���������sqlite3_index_infoָ����Ϊ�������ݡ�
**
** ������ִ���pParse���һ��������Ϣ,�򷵻ط���ֵ������
** sqlite3_index_info������Ĳ��ֱ�������䡣
**
** ���p->needToFreeIdxStr���Ǳ���Ļ�����ô�����Ƿ񷵻���һ��������
** ô��Ӧ���ɵ����������ͷ�p->idxStrָ�롣
*/
static int vtabBestIndex(Parse *pParse, Table *pTab, sqlite3_index_info *p){
  sqlite3_vtab *pVtab = sqlite3GetVTable(pParse->db, pTab)->pVtab;
  int i;
  int rc;

  WHERETRACE(("xBestIndex for %s\n", pTab->zName));
  TRACE_IDX_INPUTS(p);
  rc = pVtab->pModule->xBestIndex(pVtab, p);
  TRACE_IDX_OUTPUTS(p);

  if( rc!=SQLITE_OK ){
    if( rc==SQLITE_NOMEM ){
      pParse->db->mallocFailed = 1;
    }else if( !pVtab->zErrMsg ){
      sqlite3ErrorMsg(pParse, "%s", sqlite3ErrStr(rc));
    }else{
      sqlite3ErrorMsg(pParse, "%s", pVtab->zErrMsg);
    }
  }
  sqlite3_free(pVtab->zErrMsg);
  pVtab->zErrMsg = 0;

  for(i=0; i<p->nConstraint; i++){
    if( !p->aConstraint[i].usable && p->aConstraintUsage[i].argvIndex>0 ){
      sqlite3ErrorMsg(pParse, 
          "table %s: xBestIndex returned an invalid plan", pTab->zName);
    }
  }

  return pParse->nErr;
}


/*
** Compute the best index for a virtual table.
**
** �����������������
**
** The best index is computed by the xBestIndex method of the virtual
** table module.  This routine is really just a wrapper that sets up
** the sqlite3_index_info structure that is used to communicate with
** xBestIndex.
**
** ͨ�������ģ���xBestIndex�������������������
** �������ʵ����ֻ��һ����װ������sqlite3_index_info���ݽṹ�����ұ�������xBestIndex����ϵ
**
** In a join, this routine might be called multiple times for the
** same virtual table.  The sqlite3_index_info structure is created
** and initialized on the first invocation and reused on all subsequent
** invocations.  The sqlite3_index_info structure is also used when
** code is generated to access the virtual table.  The whereInfoDelete() 
** routine takes care of freeing the sqlite3_index_info structure after
** everybody has finished with it.
**
** ��һ�������У����������ܱ���ͬ���������ö�Ρ�
** �ڵ�һ�ε��úʹ����ͳ�ʼ��sqlite3_index_info���ݽṹ���������е����ĵ��������á�
** �����ɷ��������Ĵ���ʱ��Ҳ��ʹ��sqlite3_index_info���ݽṹ��
** �����е��˶�ִ����ɣ���ôwhereInfoDelete()������ͷ�sqlite3_index_info���ݽṹ
*/
/*
** �����������������
** 
** ���������ʹ�����ģ���xBestIndex����������ġ��������ʵ����ֻ�ǽ���
** sqlite3_index_info�ṹ�İ�װ������ṹ������xBestIndexͨ�š�
** 
** ��һ�����ӵ��У�������̿��ܻᱻͬһ�����������ɴΡ�sqlite3_index_info
** �ṹ���ڵ�һ�ε���ʱ�����ͳ�ʼ���ģ����������е��ӵ����ж����ظ�ʹ�á�
** sqlite3_index_info�ṹͬ��Ҳ�����ɴ������������ʱ��ʹ�á�
** whereInfoDelete()�����������������в�������ɺ��sqlite3_index_info�ṹ��
** �ͷš�
*/
static void bestVirtualIndex(
<<<<<<< HEAD
	Parse *pParse,                  /* The parsing context *//* ����������*/
	WhereClause *pWC,               /* The WHERE clause *//* WHERE�Ӿ�*/
struct SrcList_item *pSrc,      /* The FROM clause term to search *//* ����������FROM�Ӿ���*/
	Bitmask notReady,               /* Mask of cursors not available for index *//* ���������õ�ָ������*/
	Bitmask notValid,               /* Cursors not valid for any purpose *//* ������Ч��ָ��*/
	ExprList *pOrderBy,             /* The order by clause *//* �Ӿ�����*/
	WhereCost *pCost,               /* Lowest cost query plan *//* ��ѯ�ƻ�����С����*/
	sqlite3_index_info **ppIdxInfo  /* Index information passed to xBestIndex *//* ���͵�xBestIndex��������Ϣ*/
=======
  Parse *pParse,                  /* The parsing context ���������� */
  WhereClause *pWC,               /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,      /* The FROM clause term to search ��Ҫ��ѯ��FROM�Ӿ� */
  Bitmask notReady,               /* Mask of cursors not available for index �α��������������Ч */
  Bitmask notValid,               /* Cursors not valid for any purpose �α�����κ���;����Ч */
  ExprList *pOrderBy,             /* The order by clause ORDER BY�Ӿ� */
  WhereCost *pCost,               /* Lowest cost query plan ��С���۲���ƻ� */
  sqlite3_index_info **ppIdxInfo  /* Index information passed to xBestIndex ����xBestIndex��������Ϣ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  Table *pTab = pSrc->pTab; //��ʼ����ṹ
  sqlite3_index_info *pIdxInfo; //���ڴ洢ѡ����������Ϣ
  struct sqlite3_index_constraint *pIdxCons;  //���ڴ洢����Լ����Ϣ
  struct sqlite3_index_constraint_usage *pUsage; //�������õ�����Լ��
  WhereTerm *pTerm;
  int i, j; //i��ѭ����������j���ڴ洢
  int nOrderBy; //Order By�е�terms��
  double rCost; //�������

  /* Make sure wsFlags is initialized to some sane value. Otherwise, if the 
  ** malloc in allocateIndexInfo() fails and this function returns leaving
  ** wsFlags in an uninitialized state, the caller may behave unpredictably.
  **
  ** ȷ����ʼ��wsFlagsΪһЩ�����ֵ�����⣬�����allocateIndexInfo()�з����ڴ�ʧ�ܣ�
  ** ���������������ʣ��Ĵ���δ��ʼ��״̬��wsFlags�������ߵ���Ϊ�ǲ���Ԥ���ء�
  */
<<<<<<< HEAD
  /*
  ** ȷ��wsFlags�����������ֵ��ʼ���ġ�����Ļ������allocateIndexInfo()��
  ** �ڴ�ʧ�ܣ����������������ֵ��wsFlags����δ��ʼ��״̬����ô�����߿��ܻ�
  ** �в���Ԥ�ƵĲ�����
  */
  memset(pCost, 0, sizeof(*pCost));
  pCost->plan.wsFlags = WHERE_VIRTUALTABLE;
=======
  memset(pCost, 0, sizeof(*pCost)); //�����ڴ�
  pCost->plan.wsFlags = WHERE_VIRTUALTABLE; //��־�ƻ���ʹ���������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

  /* If the sqlite3_index_info structure has not been previously
  ** allocated and initialized, then allocate and initialize it now.
  ** ���sqlite3_index_info�ṹ��û��Ԥ�ȷ���ͳ�ʼ������ô���ھͷ���ͳ�ʼ����
  */
  /*
  ** ���sqlite3_index_info�ṹû�б�Ԥ�ȷ���ͳ�ʼ������ô�������ڽ��з���
  ** �ͳ�ʼ����
  */
  pIdxInfo = *ppIdxInfo;
  if( pIdxInfo==0 ){//���������ϢΪ����ʼ��
    *ppIdxInfo = pIdxInfo = allocateIndexInfo(pParse, pWC, pSrc, pOrderBy);//����ͳ�ʼ��������Ϣ
  }
  if( pIdxInfo==0 ){//�������ͳ�ʼ��������Ϣʧ��
    return;
  }

  /* At this point, the sqlite3_index_info structure that pIdxInfo points
  ** to will have been initialized, either during the current invocation or
  ** during some prior invocation.  Now we just have to customize the
  ** details of pIdxInfo for the current invocation and pass it to
  ** xBestIndex.
  ** 
  ** ��ʱ���ڵ�ǰ���û���һЩ��ǰ�ĵ����ڼ䣬pIdxInfoָ���sqlite3_index_info���ݽṹ������ʼ����
  ** ������ҪΪ��ǰ�����Զ���pIdxInfo�����飬���Ұ������ݸ�xBestIndex
  */
  /* ��ʱ��pIdxInfoָ���sqlite3_index_info�ṹ�ѱ���ʼ�����������ڵ�ǰ����
  ** ������֮ǰ�ĵ�������ɳ�ʼ������������ֻ��ҪΪ��ǰ���ö���pIdxInfo����
  ** ϸ�ڣ����������͵�xBestIndex��
  */

  /* The module name must be defined. Also, by this point there must
  ** be a pointer to an sqlite3_vtab structure. Otherwise
  ** sqlite3ViewGetColumnNames() would have picked up the error. 
  **
  ** ���붨��ģ������ͨ����㣬�����и�ָ��ָ��sqlite3_vtab���ݽṹ��
  ** ����sqlite3ViewGetColumnNames()���ᴦ�����
  */
<<<<<<< HEAD
  /*
  ** ����ģ������ͬʱ��������һ��ָ��ָ��sqlite3_vtab�ṹ������
  ** sqlite3ViewColumnNames()���ܻ���ִ���
  */
  assert( pTab->azModuleArg && pTab->azModuleArg[0] );
=======
  assert( pTab->azModuleArg && pTab->azModuleArg[0] ); //�����Ƿ�����ģ����
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  assert( sqlite3GetVTable(pParse->db, pTab) );

  /* Set the aConstraint[].usable fields and initialize all 
  ** output variables to zero.
  **
  ** ����aConstraint[].usable�ֶβ����������������ʼ��Ϊ0
  **
  ** aConstraint[].usable is true for constraints where the right-hand
  ** side contains only references to tables to the left of the current
  ** table.  In other words, if the constraint is of the form:
  **
  **           column = expr
  **
  ** and we are evaluating a join, then the constraint on column is 
  ** only valid if all tables referenced in expr occur to the left
  ** of the table containing column.
  **
<<<<<<< HEAD
  ** The aConstraints[] array contains for all constraints
=======
  ** aConstraint[].usable�����ұ�ֻ�������ñ���ǰ�����ߵ�Լ����TRUE.
  ** ���仰˵�����Լ����column = expr������ʽ������������һ�����ӣ�
  ** ��ô�����ϵ�Լ��ֻ�����еı�����expr���ֵı��������ߵ������ʱ����Ч��
  **
  ** The aConstraints[] array contains entries for all constraints
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  ** on the current table.  That way we only have to compute it once
  ** even though we might try to pick the best index multiple times.
  ** For each attempt at picking an index, the order of tables in the
  ** join might be different so we have to recompute the usable flag
  ** each time.
  **
  ** aConstraints[]��������ڵ�ǰ���ϵ�����Լ���ļ�¼��
  ** ������ʹ����Ӧ�ó��Զ��ȥѡ����õ�������������ֻҪ����һ�Ρ�
  ** ����ÿ�γ���ѡȡһ����������Ϊ�������еı��˳����ܲ�ͬ������ÿ��������Ҫ�ظ�������õļ���
  */
<<<<<<< HEAD
  /*
  ** ����aConstraint[].usable�򲢳�ʼ�������������Ϊ0��
  **
  ** ���ұ�ֻ����ָ��ǰ����ָ����ߵı�ʱ������Լ��������
  ** aConstraint[].usableΪ�档���仰˵�����Լ��������������ʽ��
  **
  **           column = expr
  ** ������������һ�����ӣ���ô�������expr���ἰ�ı�ָ������߰�����
  ** ��ô�е�Լ������Ϊ��Ч�ġ�
  **
  ** aConstraints����������е�ǰ��ǰ���Լ������������һ������ʹ���Ƕ�
  ** ����ͼֻȡ����������Ҳֻ��Ҫ����һ�Ρ�ÿ��ȡ�����Ĺ����У����ӵı�
  ** ��˳�����������ͬ������������Ҫÿ�ζ��ظ�������ñ�־��
  */
  pIdxCons = *(struct sqlite3_index_constraint**)&pIdxInfo->aConstraint;
  pUsage = pIdxInfo->aConstraintUsage;
  for(i=0; i<pIdxInfo->nConstraint; i++, pIdxCons++){
    j = pIdxCons->iTermOffset;
=======
  pIdxCons = *(struct sqlite3_index_constraint**)&pIdxInfo->aConstraint; //��ʼ��pIdxCons
  pUsage = pIdxInfo->aConstraintUsage; //��ʼ��pUsage
  for(i=0; i<pIdxInfo->nConstraint; i++, pIdxCons++){ //ѭ������
    j = pIdxCons->iTermOffset; 
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    pTerm = &pWC->a[j];
    pIdxCons->usable = (pTerm->prereqRight&notReady) ? 0 : 1;
  }
  memset(pUsage, 0, sizeof(pUsage[0])*pIdxInfo->nConstraint);
  if( pIdxInfo->needToFreeIdxStr ){
    sqlite3_free(pIdxInfo->idxStr);
  }
  pIdxInfo->idxStr = 0;
  pIdxInfo->idxNum = 0;
  pIdxInfo->needToFreeIdxStr = 0;
  pIdxInfo->orderByConsumed = 0;
  /* ((double)2) In case of SQLITE_OMIT_FLOATING_POINT... */
  pIdxInfo->estimatedCost = SQLITE_BIG_DBL / ((double)2);
  nOrderBy = pIdxInfo->nOrderBy;
  if( !pOrderBy ){
    pIdxInfo->nOrderBy = 0;
  }

  if( vtabBestIndex(pParse, pTab, pIdxInfo) ){
    return;
  }

  pIdxCons = *(struct sqlite3_index_constraint**)&pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    if( pUsage[i].argvIndex>0 ){
      pCost->used |= pWC->a[pIdxCons[i].iTermOffset].prereqRight;
    }
  }

  /* If there is an ORDER BY clause, and the selected virtual table index
  ** does not satisfy it, increase the cost of the scan accordingly. This
  ** matches the processing for non-virtual tables in bestBtreeIndex().
  ** �����һ��ORDER BY�Ӿ䣬����ѡ����������������ܱ�ORDER BYʹ�ã����ҵĴ�����Ӧ�����ӡ�
  ** ������bestBtreeIndex()�еķ������Ĵ��������ƥ�䡣
  */
  /* �������һ��ORDER BY�Ӿ䣬����ѡ��������������������������������
  ** ɨ��Ĵ��ۡ��������ƥ��bestBtreeIndex()�еķ�����еĹ��̡�
  */
  rCost = pIdxInfo->estimatedCost;
  if( pOrderBy && pIdxInfo->orderByConsumed==0 ){
    rCost += estLog(rCost)*rCost;
  }

  /* The cost is not allowed to be larger than SQLITE_BIG_DBL (the
  ** inital value of lowestCost in this loop. If it is, then the
  ** (cost<lowestCost) test below will never be true.
  ** 
  ** ���۲��������SQLITE_BIG_DBL(�����ѭ������ʹ��۵ĳ�ʼֵ)��
  ** �������SQLITE_BIG_DBL����ô����ıȽ�(cost<lowestCost)����Զ�Ǵ��
  **
  ** Use "(double)2" instead of "2.0" in case OMIT_FLOATING_POINT 
  ** is defined.
  **
  ** ���붨����OMIT_FLOATING_POINT��ʹ��"(double)2"����"2.0"
  */
  /* ���۲��������SQLITE_BIG_DBL(��ѭ���е�lowestCost�ĳ�ʼֵ)��
  ** ������ڣ��������(cost<lowestCost)����ֵ��Զ����Ϊ�档
  **
  ** �ڶ���OMIT_FLOATING_POINTʱʹ��"(double)2"���滻"2.0"��
  */
  if( (SQLITE_BIG_DBL/((double)2))<rCost ){
    pCost->rCost = (SQLITE_BIG_DBL/((double)2));
  }else{
    pCost->rCost = rCost;
  }
  pCost->plan.u.pVtabIdx = pIdxInfo;
  if( pIdxInfo->orderByConsumed ){
    pCost->plan.wsFlags |= WHERE_ORDERBY;
  }
  pCost->plan.nEq = 0;
  pIdxInfo->nOrderBy = nOrderBy;

  /* Try to find a more efficient access pattern by using multiple indexes
  ** to optimize an OR expression within the WHERE clause. 
  **
  ** ����ͨ��������������һ��������Ч�ķ���ģʽȥ�Ż�һ��WHERE�Ӿ��е�OR���ʽ
  */
  /* ʹ�ö���������ͼ����һ������Ч�ʵ�ģʽ�����Ż�WHERE�Ӿ��е�OR���ʽ��
  */
  bestOrClauseIndex(pParse, pWC, pSrc, notReady, notValid, pOrderBy, pCost);
}
#endif /* SQLITE_OMIT_VIRTUALTABLE */

#ifdef SQLITE_ENABLE_STAT3
/*
** Estimate the location of a particular key among all keys in an
** index.  Store the results in aStat as follows:
**
**    aStat[0]      Est. number of rows less than pVal
**    aStat[1]      Est. number of rows equal to pVal
**
** Return SQLITE_OK on success.
** ������һ�����������м��е�һ���ر����λ�á���aStat������������������:
**    aStat[0]      �е�Est. numberС��pVal
**    aStat[1]      �е�Est. number����pVal
** 
*/
/* ����һ�����������йؼ�����ĳ������ؼ��ֵ�λ�á������¸�ʽ����������
**    aStat[0]      ����С��pVal������
**    aStat[1]      ���Ƶ���pVal������
**
** ���سɹ���SQLITE_OKֵ��
*/
static int whereKeyStats(
<<<<<<< HEAD
	Parse *pParse,              /* Database connection *//*���ݿ�����*/
	Index *pIdx,                /* Index to consider domain of *//*�������������*/
	sqlite3_value *pVal,        /* Value to consider *//*�����ǵ�ֵ*/
	int roundUp,                /* Round up if true.  Round down if false *//*���Ϊ���������룬���Ϊ����������*/
	tRowcnt *aStat              /* OUT: stats written here *//*���:����д�ڴ˴�*/
=======
  Parse *pParse,              /* Database connection ���ݿ����� */
  Index *pIdx,                /* Index to consider domain of ��Ҫ���ǵ������� */
  sqlite3_value *pVal,        /* Value to consider ��Ҫ���ǵ�ֵ */
  int roundUp,                /* Round up if true.  Round down if false ���TRUE���������룬���FALSE���������� */
  tRowcnt *aStat              /* OUT: stats written here */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  tRowcnt n;
  IndexSample *aSample;
  int i, eType;
  int isEq = 0;
  i64 v;
  double r, rS;

  assert( roundUp==0 || roundUp==1 );
  assert( pIdx->nSample>0 );
  if( pVal==0 ) return SQLITE_ERROR;
  n = pIdx->aiRowEst[0];
  aSample = pIdx->aSample;
  eType = sqlite3_value_type(pVal);

  if( eType==SQLITE_INTEGER ){
    v = sqlite3_value_int64(pVal);
    r = (i64)v;
    for(i=0; i<pIdx->nSample; i++){
      if( aSample[i].eType==SQLITE_NULL ) continue;
      if( aSample[i].eType>=SQLITE_TEXT ) break;
      if( aSample[i].eType==SQLITE_INTEGER ){
        if( aSample[i].u.i>=v ){
          isEq = aSample[i].u.i==v;
          break;
        }
      }else{
        assert( aSample[i].eType==SQLITE_FLOAT );
        if( aSample[i].u.r>=r ){
          isEq = aSample[i].u.r==r;
          break;
        }
      }
    }
  }else if( eType==SQLITE_FLOAT ){
    r = sqlite3_value_double(pVal);
    for(i=0; i<pIdx->nSample; i++){
      if( aSample[i].eType==SQLITE_NULL ) continue;
      if( aSample[i].eType>=SQLITE_TEXT ) break;
      if( aSample[i].eType==SQLITE_FLOAT ){
        rS = aSample[i].u.r;
      }else{
        rS = aSample[i].u.i;
      }
      if( rS>=r ){
        isEq = rS==r;
        break;
      }
    }
  }else if( eType==SQLITE_NULL ){
    i = 0;
    if( aSample[0].eType==SQLITE_NULL ) isEq = 1;
  }else{
    assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
    for(i=0; i<pIdx->nSample; i++){
      if( aSample[i].eType==SQLITE_TEXT || aSample[i].eType==SQLITE_BLOB ){
        break;
      }
    }
    if( i<pIdx->nSample ){      
      sqlite3 *db = pParse->db;
      CollSeq *pColl;
      const u8 *z;
      if( eType==SQLITE_BLOB ){
        z = (const u8 *)sqlite3_value_blob(pVal);
        pColl = db->pDfltColl;
        assert( pColl->enc==SQLITE_UTF8 );
      }else{
        pColl = sqlite3GetCollSeq(db, SQLITE_UTF8, 0, *pIdx->azColl);
        if( pColl==0 ){
          sqlite3ErrorMsg(pParse, "no such collation sequence: %s",
                          *pIdx->azColl);
          return SQLITE_ERROR;
        }
        z = (const u8 *)sqlite3ValueText(pVal, pColl->enc);
        if( !z ){
          return SQLITE_NOMEM;
        }
        assert( z && pColl && pColl->xCmp );
      }
      n = sqlite3ValueBytes(pVal, pColl->enc);
  
      for(; i<pIdx->nSample; i++){
        int c;
        int eSampletype = aSample[i].eType;
        if( eSampletype<eType ) continue;
        if( eSampletype!=eType ) break;
#ifndef SQLITE_OMIT_UTF16
        if( pColl->enc!=SQLITE_UTF8 ){
          int nSample;
          char *zSample = sqlite3Utf8to16(
              db, pColl->enc, aSample[i].u.z, aSample[i].nByte, &nSample
          );
          if( !zSample ){
            assert( db->mallocFailed );
            return SQLITE_NOMEM;
          }
          c = pColl->xCmp(pColl->pUser, nSample, zSample, n, z);
          sqlite3DbFree(db, zSample);	//�ͷſ��ܱ�������һ���ض����ݿ����ӵ��ڴ�
        }else
#endif
        {
          c = pColl->xCmp(pColl->pUser, aSample[i].nByte, aSample[i].u.z, n, z);
        }
        if( c>=0 ){
          if( c==0 ) isEq = 1;
          break;
        }
      }
    }
  }

  /* At this point, aSample[i] is the first sample that is greater than
  ** or equal to pVal.  Or if i==pIdx->nSample, then all samples are less
  ** than pVal.  If aSample[i]==pVal, then isEq==1.
  ** 
  ** ��ʱ��aSample[i]�ǵ�һ�����ڻ����pVal��������
  ** �������i==pIdx->nSample����ô���е���������pValС��
  ** ���aSample[i]==pVal����ôisEq==1.
  */
  /* ��ʱ��aSample[i]�Ǵ��ڻ��ߵ���pValֵ�ĵ�һ�����������ߵ�ָ��
  ** i==pIdx->nSampleʱ�����е�����ֵ��С��pVal�����aSample[i]=pVal����
  ** isEq��ֵΪ1.
  */
  if( isEq ){
    assert( i<pIdx->nSample );
    aStat[0] = aSample[i].nLt;
    aStat[1] = aSample[i].nEq;
  }else{
    tRowcnt iLower, iUpper, iGap;
    if( i==0 ){
      iLower = 0;
      iUpper = aSample[0].nLt;
    }else{
      iUpper = i>=pIdx->nSample ? n : aSample[i].nLt;
      iLower = aSample[i-1].nEq + aSample[i-1].nLt;
    }
    aStat[1] = pIdx->avgEq;
    if( iLower>=iUpper ){
      iGap = 0;
    }else{
      iGap = iUpper - iLower;
    }
    if( roundUp ){
      iGap = (iGap*2)/3;
    }else{
      iGap = iGap/3;
    }
    aStat[0] = iLower + iGap;
  }
  return SQLITE_OK;
}
#endif /* SQLITE_ENABLE_STAT3 */

/*
** If expression pExpr represents a literal value, set *pp to point to
** an sqlite3_value structure containing the same value, with affinity
** aff applied to it, before returning. It is the responsibility of the 
** caller to eventually release this structure by passing it to 
** sqlite3ValueFree().
**
** ���pExpr���ʽ����һ������ֵ���ڷ���ǰ������*ppָ��һ��������ֵͬ�Ĵ����׺���affӦ����sqlite3_value���ݽṹ
** ����Ϊ���õ�����ͨ���������ݸ�sqlite3ValueFree()�������ͷ�������ݽṹ��
**
** If the current parse is a recompile (sqlite3Reprepare()) and pExpr
** is an SQL variable that currently has a non-NULL value bound to it,
** create an sqlite3_value structure containing this value, again with
** affinity aff applied to it, instead.
**
** �����ǰ������һ�����±���(sqlite3Reprepare())����pExpr��һ����ǰû�з�NULLֵ�󶨵�SQL������
** ����һ���������ֵ�Ĵ����׺���affӦ����sqlite3_value���ݽṹ��
**
** If neither of the above apply, set *pp to NULL.
**
** ��������������㣬��*pp����ΪNULLֵ
**
** If an error occurs, return an error code. Otherwise, SQLITE_OK.
**
** �������һ��������ô����һ��������룬����SQLITE_OK.
*/
/* ������ʽpExpr��ʾһ���ı�ֵ����ô��*pp��ָ�������ֵͬ��sqlite3_value
** �ṹ��ָ�룬�����ڷ���֮ǰ����������ء����հ������ݵ�sqlite3ValueFree()
** ��ʱ���ɵ��������ͷŴ˽ṹ��
**
** �����ǰ�﷨���������±����(sqlite3Reprepare())����pExpr��һ��SQL������
** ��ǰû�зǿ�ֵ��֮��أ�������һ��������ֵ��sqlite3_value�ṹ��ͬ����Ҫ
** ��֮������ء�
**
** ���û������Ӧ�ã�����*ppָ��Ϊ�ա�
**
** ������������򷵻ش�����롣���򷵻�SQLITE_OK��
*/
#ifdef SQLITE_ENABLE_STAT3
static int valueFromExpr(
  Parse *pParse, 
  Expr *pExpr, 
  u8 aff, 
  sqlite3_value **pp
){
  if( pExpr->op==TK_VARIABLE
   || (pExpr->op==TK_REGISTER && pExpr->op2==TK_VARIABLE)
  ){
    int iVar = pExpr->iColumn;
    sqlite3VdbeSetVarmask(pParse->pVdbe, iVar);
    *pp = sqlite3VdbeGetValue(pParse->pReprepare, iVar, aff);
    return SQLITE_OK;
  }
  return sqlite3ValueFromExpr(pParse->db, pExpr, SQLITE_UTF8, aff, pp);
}
#endif

/*
** This function is used to estimate the number of rows that will be visited
** by scanning an index for a range of values. The range may have an upper
** bound, a lower bound, or both. The WHERE clause terms that set the upper
** and lower bounds are represented by pLower and pUpper respectively. For
** example, assuming that index p is on t1(a):
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**                    |_____|   |_____|
**                       |         |
**                     pLower    pUpper
**
** If either of the upper or lower bound is not present, then NULL is passed in
** place of the corresponding WhereTerm.
**
** ����������ڹ���ͨ��ɨ��һ��������ȡһϵ��ֵ����Ҫ���ʵ�������
** �����Χ���������ޣ����޻��߶��С�WHERE�Ӿ�termsͨ��pLower��pUpper�ֱ��������޺����޵�ֵ��
** ���룬��������p����t1(a)��:
**   ... FROM t1 WHERE a > ? AND a < ? ...
**                    |_____|   |_____|
**                       |         |
**                     pLower    pUpper
** ���û�������޻����ޣ���ô����NULLֵ������Ӧ��WhereTerm.
**
** The nEq parameter is passed the index of the index column subject to the
** range constraint. Or, equivalently, the number of equality constraints
** optimized by the proposed index scan. For example, assuming index p is
** on t1(a, b), and the SQL query is:
**
**   ... FROM t1 WHERE a = ? AND b > ? AND b < ? ...
**
** then nEq should be passed the value 1 (as the range restricted column,
** b, is the second left-most column of the index). Or, if the query is:
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**
** then nEq should be passed 0.
**
** nEq������Ҫ����Ϊ�����е��±���ӷ�ΧԼ������ȼ۵أ�ͨ���Ƽ�������ɨ���Ż���ʽԼ���ĸ�����
** ���磬��������p����t1(a, b)�У�SQL��ѯ��:
**   ... FROM t1 WHERE a = ? AND b > ? AND b < ? ...
** ��ônEq����Ϊֵ1(��Ϊ��Χ���޵��У�b���������ڶ�������ߵ���).�������ѯ��:
**   ... FROM t1 WHERE a > ? AND a < ? ...
** ��ônEq����Ϊֵ0��
**
** The returned value is an integer divisor to reduce the estimated
** search space.  A return value of 1 means that range constraints are
** no help at all.  A return value of 2 means range constraints are
** expected to reduce the search space by half.  And so forth...
**
** ����ֵ��һ����������������Ԥ�Ƶ������ռ䡣
** һ��ֵΪ1�ķ���ֵ��ζ�ŷ�ΧԼ����û�а����ġ�
** һ��ֵΪ2�ķ���ֵ��ζ�ŷ�ΧԼ������������һ��������ռ䡣�ȵ�...
**
** In the absence of sqlite_stat3 ANALYZE data, each range inequality
** reduces the search space by a factor of 4.  Hence a single constraint (x>?)
** results in a return of 4 and a range constraint (x>? AND x<?) results
** in a return of 16.
**
** ȱ��sqlite_stat3 ANALYZE���ݣ�ÿ����Χ����ʽ������4���������ռ䡣
** ���һ��������Լ��(x>?)���·���4����һ����ΧԼ��(x>? AND x<?)���·���16��
*/
/* �˺�����������ɨ��һ����Χ��ֵ�������У�������ʵ��������������Χ���ܻ���
** һ�����ޣ�һ�����ޣ��������߶��С�WHERE�Ӿ����У���pLower��pUpper����ʾ�趨
** �����±߽硣���磬����p������t1(a):
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**                    |_____|   |_____|
**                       |         |
**                     pLower    pUpper
**
** ������޺������е�����һ�������ڣ���WhereTerm�ж�Ӧ�����ÿ�ֵ���滻��
**
** ����nEq��ͨ�������е�������Χ��Լ���ġ�����ͬ���أ������������ɨ�����Ż���
** ֵԼ�������磬��������p��t1(a,b),��SQL��ѯΪ��
**
**   ... FROM t1 WHERE a = ? AND b > ? AND b < ? ...
**
** ��ônEq��ֵΪ1(��Χ������b���ǵڶ���ָ������ߵ���)�����ߣ������ѯΪ��
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**
** ��ônEq��ֵΪ0��
**
** ����ֵ��һ�����������������ٹ��Ƶ������ռ䡣����ֵΪ1��ζ�ŷ�ΧԼ����û�����塣
** ����ֵΪ2��ζ�ŷ�ΧԼ��Ԥ�ƽ�����һ��������ռ䡣�ȵȡ���
**
** ��ȱ��sqlite_stat3�������ݵ�����£�ÿ����ƽ�����ؼ����������ռ�ķ�Χ4�����
** һ��Լ��(x>?)����ķ���4��һϵ��Լ��(x>?��x<?)�������16��
*/
static int whereRangeScanEst(
<<<<<<< HEAD
	Parse *pParse,       /* Parsing & code generating context *//*���������Ĳ������ɴ���*/
	Index *p,            /* The index containing the range-compared column; "x" *//*���������Աȷ�Χ��"x"*/
	int nEq,             /* index into p->aCol[] of the range-compared column *//*ָ��Աȷ�Χ�е�����ָ��p_>aCol[]*/
	WhereTerm *pLower,   /* Lower bound on the range. ex: "x>123" Might be NULL *//*��Χ���½磺����"x>123"����Ϊ��*/
	WhereTerm *pUpper,   /* Upper bound on the range. ex: "x<455" Might be NULL *//*��Χ���Ͻ磺����"x<455"����Ϊ��*/
	double *pRangeDiv   /* OUT: Reduce search space by this divisor *//*��������������������������ռ�*/
=======
  Parse *pParse,       /* Parsing & code generating context �������Ҵ������������� */
  Index *p,            /* The index containing the range-compared column; "x" ����������Χ���յ���:"x" */
  int nEq,             /* index into p->aCol[] of the range-compared column ����ָ��Χ�����е�p->aCol[] */
  WhereTerm *pLower,   /* Lower bound on the range. ex: "x>123" Might be NULL �ڷ�Χ�е�����:ex: "x>123"������NULL */
  WhereTerm *pUpper,   /* Upper bound on the range. ex: "x<455" Might be NULL �ڷ�Χ�е�����:ex: "x<455"������NULL */
  double *pRangeDiv   /* OUT: Reduce search space by this divisor OUT:ͨ��������������������ռ� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  int rc = SQLITE_OK;

#ifdef SQLITE_ENABLE_STAT3

  if( nEq==0 && p->nSample ){
    sqlite3_value *pRangeVal;
    tRowcnt iLower = 0;
    tRowcnt iUpper = p->aiRowEst[0];
    tRowcnt a[2];
    u8 aff = p->pTable->aCol[p->aiColumn[0]].affinity;

    if( pLower ){
      Expr *pExpr = pLower->pExpr->pRight;
      rc = valueFromExpr(pParse, pExpr, aff, &pRangeVal);
      assert( pLower->eOperator==WO_GT || pLower->eOperator==WO_GE );
      if( rc==SQLITE_OK
       && whereKeyStats(pParse, p, pRangeVal, 0, a)==SQLITE_OK
      ){
        iLower = a[0];
        if( pLower->eOperator==WO_GT ) iLower += a[1];
      }
      sqlite3ValueFree(pRangeVal);
    }
    if( rc==SQLITE_OK && pUpper ){
      Expr *pExpr = pUpper->pExpr->pRight;
      rc = valueFromExpr(pParse, pExpr, aff, &pRangeVal);
      assert( pUpper->eOperator==WO_LT || pUpper->eOperator==WO_LE );
      if( rc==SQLITE_OK
       && whereKeyStats(pParse, p, pRangeVal, 1, a)==SQLITE_OK
      ){
        iUpper = a[0];
        if( pUpper->eOperator==WO_LE ) iUpper += a[1];
      }
      sqlite3ValueFree(pRangeVal);
    }
    if( rc==SQLITE_OK ){
      if( iUpper<=iLower ){
        *pRangeDiv = (double)p->aiRowEst[0];
      }else{
        *pRangeDiv = (double)p->aiRowEst[0]/(double)(iUpper - iLower);
      }
      WHERETRACE(("range scan regions: %u..%u  div=%g\n",
                  (u32)iLower, (u32)iUpper, *pRangeDiv));
      return SQLITE_OK;
    }
  }
#else
  UNUSED_PARAMETER(pParse);
  UNUSED_PARAMETER(p);
  UNUSED_PARAMETER(nEq);
#endif
  assert( pLower || pUpper );
  *pRangeDiv = (double)1;
  if( pLower && (pLower->wtFlags & TERM_VNULL)==0 ) *pRangeDiv *= (double)4;
  if( pUpper ) *pRangeDiv *= (double)4;
  return rc;
}

#ifdef SQLITE_ENABLE_STAT3
/*
** Estimate the number of rows that will be returned based on
** an equality constraint x=VALUE and where that VALUE occurs in
** the histogram data.  This only works when x is the left-most
** column of an index and sqlite_stat3 histogram data is available
** for that index.  When pExpr==NULL that means the constraint is
** "x IS NULL" instead of "x=VALUE".
**
** ���ƽ��᷵�ص����������ǻ���һ����ʽԼ��x=VALUE����VALUE������ֱ��ͼ�е�.
** ��ֻ���ڵ�x��һ������������ߵ��в���sqlite_stat3ֱ��ͼ���ݶ�����������Чʱ�������á�
** ��pExpr==NULL��ζ��Լ������"x IS NULL"����"x=VALUE".
**
** Write the estimated row count into *pnRow and return SQLITE_OK. 
** If unable to make an estimate, leave *pnRow unchanged and return
** non-zero.
**
** �ѹ��Ƶ�����д�뵽*pnRow�в��ҷ���SQLITE_OK.
** ���������һ�����ƣ�����*pnRow���䲢�ҷ��ط�0ֵ��
**
** This routine can fail if it is unable to load a collating sequence
** required for string comparison, or if unable to allocate memory
** for a UTF conversion required for comparison.  The error is stored
** in the pParse structure.
**
** �����������ܴ��ַ����Ƚ���������ص�һ��˳�����У�
** ����������ܴӱȽ�������һ��UTF(Unicodeת��ģʽ)ת������ô��������ʧ�ܡ�
** ������Ϣ��洢��pParse���ݽṹ��
*/
/* 
** ���ƻ��ڵ�ֵԼ������x=VALUE���ص���������VALUEֵ����״ͼ�����г��֡�
** ���ֻ��x������������ߵ��в���sqlite_stat3��״���ݶ��ڴ��б��ǿ���
** ������²������á���pExprֵΪ��ʱ����ζ��Լ��������"xΪ��"������"x��
** ֵΪVALUE"��
** 
** ��¼���Ƶ�����������ֵд��*pnRowȻ�󷵻�SQLITE_OK����������������ƣ�
** �򱣳�*pnRowԭ��ֵ���ҷ��ط��㡣
**
** ������ܹ������ַ����Ƚ�������������У����߲��ܹ��Ƚ�ʱ�����UTF�Ự
** �����ڴ�ռ䣬������̿�������ʧ�ܡ�������������pParse�ṹ�С�
*/
static int whereEqualScanEst(
<<<<<<< HEAD
	Parse *pParse,       /* Parsing & code generating context *//*���������Ĳ������ɴ���*/
	Index *p,            /* The index whose left-most column is pTerm *//*��������pTerm������*/
	Expr *pExpr,         /* Expression for VALUE in the x=VALUE constraint *//*x=VALUEԼ��������VALUE�ı��ʽ*/
	double *pnRow        /* Write the revised row estimate here *//*�ڴ�д�������޸ĵ��еĹ���ֵ*/
){
	sqlite3_value *pRhs = 0;  /* VALUE on right-hand side of pTerm *//*pTerm�ұ����ֵ*/
	u8 aff;                   /* Column affinity *//*ͬ����*/
	int rc;                   /* Subfunction return code *//*�Ӻ������ش���*/
	tRowcnt a[2];             /* Statistics *//*ͳ������*/
=======
  Parse *pParse,       /* Parsing & code generating context ���������ĺ����ɴ��� */
  Index *p,            /* The index whose left-most column is pTerm pTerm�������е����� */
  Expr *pExpr,         /* Expression for VALUE in the x=VALUE constraint ��x=VALUEԼ���е�VALUE���ʽ */
  double *pnRow        /* Write the revised row estimate here д���޸ĺ�Ĺ��Ƶ��� */
){
  sqlite3_value *pRhs = 0;  /* VALUE on right-hand side of pTerm ��pTerm�ұߵ�VALUE */
  u8 aff;                   /* Column affinity ���׺��� */
  int rc;                   /* Subfunction return code ���ش�����Ӻ��� */
  tRowcnt a[2];             /* Statistics ͳ����Ϣ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

  assert( p->aSample!=0 );
  assert( p->nSample>0 );
  aff = p->pTable->aCol[p->aiColumn[0]].affinity;
  if( pExpr ){
    rc = valueFromExpr(pParse, pExpr, aff, &pRhs);
    if( rc ) goto whereEqualScanEst_cancel;
  }else{
    pRhs = sqlite3ValueNew(pParse->db);
  }
  if( pRhs==0 ) return SQLITE_NOTFOUND;
  rc = whereKeyStats(pParse, p, pRhs, 0, a);
  if( rc==SQLITE_OK ){
    WHERETRACE(("equality scan regions: %d\n", (int)a[1]));
    *pnRow = a[1];
  }
whereEqualScanEst_cancel:
  sqlite3ValueFree(pRhs);
  return rc;
}
#endif /* defined(SQLITE_ENABLE_STAT3) *//*����(SQLITE_ENABLE_STAT3)����*/

#ifdef SQLITE_ENABLE_STAT3
/*
** Estimate the number of rows that will be returned based on
** an IN constraint where the right-hand side of the IN operator
** is a list of values.  Example:
**
**        WHERE x IN (1,2,3,4)
**
** Write the estimated row count into *pnRow and return SQLITE_OK. 
** If unable to make an estimate, leave *pnRow unchanged and return
** non-zero.
**
** ���ƽ�Ҫ���ص��������ǻ���һ��INԼ����INԤ������ұ���һϵ��ֵ������:
**        WHERE x IN (1,2,3,4)
** �ѹ��Ƶ�����д�뵽*pnRow���ҷ���SQLITE_OK��
** ���������һ�����ƣ�����*pnRow���䲢�ҷ��ط�0ֵ��
**
** This routine can fail if it is unable to load a collating sequence
** required for string comparison, or if unable to allocate memory
** for a UTF conversion required for comparison.  The error is stored
** in the pParse structure.
**
** �����������ܴ��ַ����Ƚ���������ص�һ��˳�����У�
** ����������ܴӱȽ�������һ��UTF(Unicodeת��ģʽ)ת������ô��������ʧ�ܡ�
** ������Ϣ��洢��pParse���ݽṹ��

*/
/*
** ���ƻ���INԼ�������ķ��ص����������Լ��������IN��������ұ�Ϊһ��
** ֵ�����磺
**
**        WHERE x IN (1,2,3,4)
**
** �Թ���������������д��*pnRow��Ȼ�󷵻�SQLITE_OK����������������ƣ�
** �򱣳�*pnRowΪԭֵ���ҷ��ط��㡣
**
** ������ܹ������ַ����Ƚ�������������У����߲��ܹ��Ƚ�ʱ�����UTF�Ự
** �����ڴ�ռ䣬������̿�������ʧ�ܡ�������������pParse�ṹ�С�
*/
static int whereInScanEst(
<<<<<<< HEAD
	Parse *pParse,       /* Parsing & code generating context *//*���������Ĳ������ɴ���*/
	Index *p,            /* The index whose left-most column is pTerm *//*�������ΪpTerm������*/
	ExprList *pList,     /* The value list on the RHS of "x IN (v1,v2,v3,...)" *//*xȡֵ(v1,v2,v3...)��RHS��ֵ���б�*/
  double *pnRow        /* Write the revised row estimate here *//*�ڴ�д�������޸ĵ��еĹ���ֵ*/
){
	int rc = SQLITE_OK;         /* Subfunction return code *//*�Ӻ������ش���*/
	double nEst;                /* Number of rows for a single term *//*��һ�������*/
	double nRowEst = (double)0; /* New estimate of the number of rows *//*�������¹���ֵ*/
	int i;                      /* Loop counter *//*ѭ��������*/
=======
  Parse *pParse,       /* Parsing & code generating context ���������ɴ��������� */
  Index *p,            /* The index whose left-most column is pTerm pTerm�������е����� */
  ExprList *pList,     /* The value list on the RHS of "x IN (v1,v2,v3,...)" "x IN (v1,v2,v3,...)"�ұߵ�һϵ��ֵ */
  double *pnRow        /* Write the revised row estimate here д���޸ĺ�Ĺ��Ƶ��� */
){
  int rc = SQLITE_OK;         /* Subfunction return code ���ش�����Ӻ��� */
  double nEst;                /* Number of rows for a single term ����һ��term������ */
  double nRowEst = (double)0; /* New estimate of the number of rows �¹��Ƶ����� */
  int i;                      /* Loop counter ѭ�������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

  assert( p->aSample!=0 );
  for(i=0; rc==SQLITE_OK && i<pList->nExpr; i++){
    nEst = p->aiRowEst[0];
    rc = whereEqualScanEst(pParse, p, pList->a[i].pExpr, &nEst);
    nRowEst += nEst;
  }
  if( rc==SQLITE_OK ){
    if( nRowEst > p->aiRowEst[0] ) nRowEst = p->aiRowEst[0];
    *pnRow = nRowEst;
    WHERETRACE(("IN row estimate: est=%g\n", nRowEst));
  }
  return rc;
}
//�ϸӱ����

/*���㳬 �Ӵ˿�ʼ
** Find the best query plan for accessing a particular table.  Write the
** best query plan and its cost into the WhereCost object supplied as the
** last parameter.
<<<<<<< HEAD
**Ϊĳ���ض��ı�Ѱ����Ѳ�ѯ�ƻ���ȷ����õĲ�ѯ�ƻ��ͳɱ���
д��WhereCost������Ϊ���һ�������ṩ
=======
**
** ѡ�����һ���ر�����õĲ�ѯ�ƻ���
** ����õĲ�ѯ�Ż������Ĵ���д��WhereCost���󣬲�����Ϊ���Ĳ����ṩ��
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** The lowest cost plan wins.  The cost is an estimate of the amount of
** CPU and disk I/O needed to process the requested result.
��ʹ���ԭ�򣬴����ǵ�һ�����Ƶ� CPU �ʹ��� I/O ������̵Ľ�������ܺ�
** Factors that influence cost include:
Ӱ����۵�������:
**
**    *  The estimated number of rows that will be retrieved.  (The
**       fewer the better.)
**��ѯ��ȡ�ļ�¼�� (Խ��Խ��)
**    *  Whether or not sorting must occur.
** ����Ƿ�����.
**    *  Whether or not there must be separate lookups in the
**       index and in the main table.
<<<<<<< HEAD
**�Ƿ���Ҫ����������ԭ��
=======
**
** ��С���۵ļƻ������á������Ƕ���Ҫ���������CPU�ʹ���I/O�������Ĺ��㡣
** Ӱ����۵����ذ���:
**    *  ���㽫����ȡ�ص�����
**
**    *  �����Ƿ�Ӧ�÷���
**
**    *  ���������������Ƿ�Ӧ�÷ֿ�����
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** If there was an INDEXED BY clause (pSrc->pIndex) attached to the table in
** the SQL statement, then this function only considers plans using the 
** named index. If no such plan is found, then the returned cost is
** SQLITE_BIG_DBL. If a plan is found that uses the named index, 
** then the cost is calculated in the usual way.
<<<<<<< HEAD
**���û������ BY �Ӿ� ��pSrc->pIndex�� ���ӵ� SQL ����еı�
�˺���ֻ���Ǽƻ�ʹ��ָ����������
���û�������ļƻ��ҵ�����ô���صĳɱ����� SQLITE_BIG_DBL��
���һ��ƻ����ҵ�ʹ��ָ����������Ȼ��ͨ����ʽ������ۡ�
=======
**
** �����SQL�����б����һ��INDEXED BY(pSrc->pIndex)����ô�������ֵֻ����ʹ��ָ����������
** ���û���ҵ������ļƻ�����ô���صĴ�����SQLITE_BIG_DBL.
** ����ҵ���ʹ��ָ�������ļƻ�����ô��ƽ���ķ�ʽ������ۡ�
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** If a NOT INDEXED clause (pSrc->notIndexed!=0) was attached to the table 
** in the SELECT statement, then no indexes are considered. However, the 
** selected plan may still take advantage of the built-in rowid primary key
** index.
<<<<<<< HEAD
���û�������Ӿ� ��pSrc->notIndexed!=0�� ���ӵ��ı��е� SELECT ��䣬��Ϊû��������
Ȼ������ѡ�ļƻ���Ȼ�����������õ� rowid ����������
=======
**
** �����SELECT�����б����һ��NOT INDEXED�Ӿ�(pSrc->notIndexed!=0)����ô��Ϊ��û�������ġ�
** Ȼ������ѯ�ƻ������Ծ����ô�����rowid�ϵ���������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
*/
//���pCost��bestBtreeIndex����������ѯ������Ϣ����Ӧ�Ĵ���
static void bestBtreeIndex(
<<<<<<< HEAD
  Parse *pParse,              /* ���������� */
  WhereClause *pWC,           /* where�Ӿ�  */
  struct SrcList_item *pSrc,  /*  FROM�Ӿ�������� */
  Bitmask notReady,           /* ���������������α����� */
  Bitmask notValid,           /* ���������κ���;���α� */
  ExprList *pOrderBy,         /* ORDER BY �Ӿ� */
  ExprList *pDistinct,        /* ����DISTINCT��ѡ���б�  */
  WhereCost *pCost            /* ������С��ѯ���� */
){
  int iCur = pSrc->iCursor;   /* Ҫ���ʱ���α� */
  Index *pProbe;              /* �������ڲ��Ե����� */
  Index *pIdx;                /* pProbe�ĸ�����������IPK����*/
  int eqTermMask;             /* ��ǰ�������Ч����ͬ������ */
  int idxEqTermMask;          /* ��Ч����ͬ���������������� */
  Index sPk;                  /* һ����������������� */
  tRowcnt aiRowEstPk[2];      /* sPk ������aiRowEst[] ֵ*/
  int aiColumnPk = -1;        /* sPk ������aColumn[]ֵ */
  int wsFlagMask;             /* pCost->plan.wsFlag�е������־ */

  /* Initialize the cost to a worst-case value 
  ��ʼ������������ֵ
  */
=======
  Parse *pParse,              /* The parsing context ���������� */
  WhereClause *pWC,           /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,  /* The FROM clause term to search ���в��ҵ�FROM�Ӿ�term */
  Bitmask notReady,           /* Mask of cursors not available for indexing �α����������������Ч�� */
  Bitmask notValid,           /* Cursors not available for any purpose �����κ�Ŀ���α궼��Ч */
  ExprList *pOrderBy,         /* The ORDER BY clause ORDER BY�Ӿ� */
  ExprList *pDistinct,        /* The select-list if query is DISTINCT �����ѯ��DISTINCTʱ��select-list */
  WhereCost *pCost            /* Lowest cost query plan ��С�Ĵ��۲�ѯ�ƻ� */
){
  int iCur = pSrc->iCursor;   /* The cursor of the table to be accessed ��ȡ�ı��α� */
  Index *pProbe;              /* An index we are evaluating ����Ҫ������һ������ */
  Index *pIdx;                /* Copy of pProbe, or zero for IPK index ��pProbe�ĸ�����0��IPK���� */
  int eqTermMask;             /* Current mask of valid equality operators ��ǰ��Ч��ʽ����������� */
  int idxEqTermMask;          /* Index mask of valid equality operators ��ǰ��Ч��ʽ��������������� */
  Index sPk;                  /* A fake index object for the primary key һ��������α����������� */
  tRowcnt aiRowEstPk[2];      /* The aiRowEst[] value for the sPk index sPk������aiRowEst[]ֵ */
  int aiColumnPk = -1;        /* The aColumn[] value for the sPk index sPk������aColumn[]ֵ */
  int wsFlagMask;             /* Allowed flags in pCost->plan.wsFlag ��pCost->plan.wsFlag����ı�־ */

  /* Initialize the cost to a worst-case value ��ʼ���ɱ�Ϊworst-caseֵ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  memset(pCost, 0, sizeof(*pCost));
  pCost->rCost = SQLITE_BIG_DBL;

  /* If the pSrc table is the right table of a LEFT JOIN then we may not
  ** use an index to satisfy IS NULL constraints on that table.  This is
  ** because columns might end up being NULL if the table does not match -
  ** a circumstance which the index cannot help us discover.  Ticket #2177.
<<<<<<< HEAD
  ���pSrc���������ӣ����ǾͲ���������Ϊ����������ϣ�������Ϊ�����ƥ�䣬
  �п������ջᱻ��NULLֵ����������������Ͳ��ܰ������ǽ��в��ҡ�
=======
  **
  ** ���pSrc��ʾһ�������ӵ��ұ���ô�������Ǹ����Ͽ��ܲ�ʹ��������IS NULLԼ���ϡ�
  ** ������Ϊ��������������ܰ������Ƿ���һ�������ƥ���ǣ���ô�п�����NULL��β�� Ticket #2177.
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  //
  if( pSrc->jointype & JT_LEFT ){
    idxEqTermMask = WO_EQ|WO_IN;
  }else{
    idxEqTermMask = WO_EQ|WO_IN|WO_ISNULL;
  }

  if( pSrc->pIndex ){
<<<<<<< HEAD
    /* ����BY�Ӿ�ָ����һ���ض���������ʹ�� */
=======
    /* An INDEXED BY clause specifies a particular index to use һ��INDEXED BY�Ӿ�ָ��ʹ��һ���ر������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    pIdx = pProbe = pSrc->pIndex;
    wsFlagMask = ~(WHERE_ROWID_EQ|WHERE_ROWID_RANGE);
    eqTermMask = idxEqTermMask;
  }else{
    /* There is no INDEXED BY clause.  Create a fake Index object in local
    ** variable sPk to represent the rowid primary key index.  Make this
    ** fake index the first in a chain of Index objects with all of the real
    ** indices to follow 
<<<<<<< HEAD
    ����û������BY�Ӿ䡣�ھֲ���������һ�������������
    sPk����rowid����������
    ʹ��������������ĵ�һ�������������ʵ��
    */
    Index *pFirst;                  /* ���е�һ������������� */
=======
    ** 
    ** û��INDEXED BY�Ӿ䡣�ھֲ�����sPk�д���һ����ʾrowid������α������
    ** ʹ���α��������һ������ʵ��������ĵ�һ��λ��
    */
    Index *pFirst;                  /* First of real indices on the table �ڱ�����ʵ�����ĵ�һ�� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    memset(&sPk, 0, sizeof(Index));
    sPk.nColumn = 1;
    sPk.aiColumn = &aiColumnPk;
    sPk.aiRowEst = aiRowEstPk;
    sPk.onError = OE_Replace;
    sPk.pTable = pSrc->pTab;
    aiRowEstPk[0] = pSrc->pTab->nRowEst;
    aiRowEstPk[1] = 1;
    pFirst = pSrc->pTab->pIndex;
    if( pSrc->notIndexed==0 ){
      /* The real indices of the table are only considered if the
      ** NOT INDEXED qualifier is omitted from the FROM clause
<<<<<<< HEAD
      ���FROM�Ӿ�û���޶�������ֻ����ʵ�ʵ����� */
=======
      ** �����ʵ����ֻ������NOT INDEXED�޶���FROM�Ӿ��б�ɾ��ʱ�ſ��ǡ�
      */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      sPk.pNext = pFirst;
    }
    pProbe = &sPk;
    wsFlagMask = ~(
        WHERE_COLUMN_IN|WHERE_COLUMN_EQ|WHERE_COLUMN_NULL|WHERE_COLUMN_RANGE
    );
    eqTermMask = WO_EQ|WO_IN;
    pIdx = 0;
  }

  /* Loop over all indices looking for the best one to use
<<<<<<< HEAD
  ��������������,�ҵ�һ��������С������
=======
  ** ѭ�����е�������������õ�һ����ʹ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  for(; pProbe; pIdx=pProbe=pProbe->pNext){ //ѭ����������
    const tRowcnt * const aiRowEst = pProbe->aiRowEst;
<<<<<<< HEAD
    double cost;                /* ʹ��pProbe�Ĵ��� */
    double nRow;                /* Ԥ�ƽ�����еļ�¼�� */
    double log10N = (double)1;  /*ʮ���ƻ��� (����ȷ) */
    int rev;                    /* ������ȷɨ�� */
=======
    double cost;                /* Cost of using pProbe pProbeʹ�õĴ��� */
    double nRow;                /* Estimated number of rows in result set �ڽ�����й������� */
    double log10N = (double)1;  /* base-10 logarithm of nRow (inexact) nRow����10Ϊ�׵Ķ���(����ȷ��) */
    int rev;                    /* True to scan in reverse order �ڵ�������ȷ��ɨ�� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    int wsFlags = 0;
    Bitmask used = 0;

    /* The following variables are populated based on the properties of
    ** index being evaluated. They are then used to determine the expected
    ** cost and number of rows returned.
<<<<<<< HEAD
    **�������ı����������Ļ������������ԡ�
    ����Ȼ��ʹ��ȷ����Ԥ�ڴ��ۺͷ��صļ�¼����
=======
    **
    ** ���еı����ǻ��������������������������ġ����Ǿ�����������Ԥ�ڵĴ��ۺ�������
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  nEq: 
    **    Number of equality terms that can be implemented using the index.
    **    In other words, the number of initial fields in the index that
    **    are used in == or IN or NOT NULL constraints of the WHERE clause.
<<<<<<< HEAD
    **������ȿ�������������ʵ�֡�
    ���仰˵,���������������==
    ����WHERE�Ӿ��NOT NULLԼ��
=======
    **
    **  nEq:
    **	 ��ʹ�������ĵ�ʽterms�ĸ��������仰˵���������еĳ�ʼ�����ֶ�������WHERE�Ӿ��е�==��IN��NOT NULL
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  nInMul:  
    **    The "in-multiplier". This is an estimate of how many seek operations 
    **    SQLite must perform on the index in question. For example, if the 
    **    WHERE clause is:
    **��in-multiplier��������һ�������ж��ٲ�����SQLite����ִ�����������⣬
    ���� where�Ӿ�
    **      WHERE a IN (1, 2, 3) AND b IN (4, 5, 6)
    **
    **    SQLite must perform 9 lookups on an index on (a, b), so nInMul is 
    **    set to 9. Given the same schema and either of the following WHERE 
    **    clauses:
    **SQLite����ִ��9�β����ڰɣ�a��b���С�����nInMul��Ϊ9��
    �������һ����ͬ���ҵ�where�Ӿ�
    **      WHERE a =  1
    **      WHERE a >= 2
    **
    **    nInMul is set to 1.
    **
    **    If there exists a WHERE term of the form "x IN (SELECT ...)", then 
    **    the sub-select is assumed to return 25 rows for the purposes of 
    **    determining nInMul.
<<<<<<< HEAD
    **����������һ��where�Ӿ䣨x IN (SELECT ...))����ô�Ӳ�ѯ����Ϊ25��,��nInMul��ֵ
=======
    **
    **
    **  nInMul:
    **    "in-multiplier".���ǹ���SQLite�������е�������ִ���˶�������������
    **	 ���磬���WHERE�Ӿ���;
    **      WHERE a IN (1, 2, 3) AND b IN (4, 5, 6)
    **    SQLite������(a, b)�ϵ�����ִ��9�β��ң����nInMul��Ϊ9.�����һ�����Ƶ�����;
    **      WHERE a =  1
    **      WHERE a >= 2
    **    nInMul����Ϊ1.
    **
    **    �������һ����ʽΪ"x IN (SELECT ...)"��WHERE term����ô�Ӳ�ѯΪ��ȷ��nInMul�ͼٶ�����25�С�
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  bInEst:  
    **    Set to true if there was at least one "x IN (SELECT ...)" term used 
    **    in determining the value of nInMul.  Note that the RHS of the
    **    IN operator must be a SELECT, not a value list, for this variable
    **    to be true.
<<<<<<< HEAD
    **����Ϊ��,���������һ����ѯ�Ӿ������ھ���nInMul��ֵ��
    ע��,�ڲ�����������һ��ѡ��,������һ��ֵ�б�,�����������ʵ�ġ�
=======
    **
    **  bInEst:  
    **    ���������һ��"x IN (SELECT ...)" term���ھ���nInMul��ֵ����ô������ΪTRUE.
    **    ע��:IN��������ұ߶����������������һ����ʵ��SELECT��������һ��ֵ�б�
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  rangeDiv:
    **    An estimate of a divisor by which to reduce the search space due
    **    to inequality constraints.  In the absence of sqlite_stat3 ANALYZE
    **    data, a single inequality reduces the search space to 1/4rd its
    **    original size (rangeDiv==4).  Two inequalities reduce the search
    **    space to 1/16th of its original size (rangeDiv==16).
<<<<<<< HEAD
    **���Ƶ�һ������,���������ռ�ȡ���ڲ���ʽԼ����
    ��ȱ��sqlite_stat3�������ݵ������,
    ��һ��ƽ�ȵĽ��������ռ���ԭʼ��С1/4(rangeDiv = = 4)��
    ������ƽ�Ƚ��������ռ��ŵ�ԭʼ��С��1/16(rangeDiv = = 16)
=======
    **
    **  rangeDiv:
    **    һ�����ڲ���ʽԼ�������������ռ������������
    **    ȱ��sqlite_stat3 ANALYZE�����ݣ�һ�������Ĳ���ʽ�������ռ���ٵ�ԭʼ�Ĵ�С��1/4(rangeDiv==4).
    **    ��������ʽ�������ռ���ٵ�ԭʼ��С��1/16(rangeDiv==16).
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  bSort:   
    **    Boolean. True if there is an ORDER BY clause that will require an 
    **    external sort (i.e. scanning the index being evaluated will not 
    **    correctly order records).
<<<<<<< HEAD
    **����ֵΪ�棬�����һ��ORDER BY�Ӿ�,��Ҫһ���ⲿ����(��ɨ����������������ȷ�����¼)��
=======
    **
    **  bSort:  
    **    Boolean����.�����һ��ORDER BY�Ӿ�Ҫ��һ���ⲿ����(Ҳ����˵��ɨ��������������������ȷ�������¼)�򷵻�TRUE
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **  bLookup: 
    **    Boolean. True if a table lookup is required for each index entry
    **    visited.  In other words, true if this is not a covering index.
    ����ֵΪ�棬���һ���������Ҫ����ÿ�������
    ���仰˵,����ⲻ��һ�������ĸ���������
    **    This is always false for the rowid primary key index of a table.
    **    For other indexes, it is true unless all the columns of the table
    **    used by the SELECT statement are present in the index (such an
    **    index is sometimes described as a covering index).
    �����Ǵ����rowid����������
    ��������,�������,����ʹ�õı�������е�SELECT����������
    (������ʱ������Ϊһ����������)��
    **    For example, given the index on (a, b), the second of the following 
    **    two queries requires table b-tree lookups in order to find the value
    **    of column c, but the first does not because columns a and b are
    **    both available in the index.
    **���磬������a��b���ڶ�����ѯ��Ҫ����b�����ң�Ϊ���ҵ���c��ֵ,
    ����һ��ѯ������Ҫ����Ϊ��a��b���ǿ��õ�������
    **             SELECT a, b    FROM tbl WHERE a = 1;
    **             SELECT a, b, c FROM tbl WHERE a = 1;
    **
    **  bLookup: 
    **    Boolean����.���һ�����ѯ��Ҫ����ÿһ��������Ŀ����bLookup����TRUE.
    **    ���仰˵������ⲻ��һ�������������򷵻�TRUE.����һ�����rowid�����������Ǵ���ġ�
    **    ����������������
    **    ������SELECT�����б�������ж�������������(����������ʱ��˵����һ����������)����ôbLookup����TRUE
    **    ���磬��(a, b)��������������������ѯ��������b-tree����Ϊ�˲�����c��ֵ��
    **    ������Ϊ��a��b�����������еı��������Ե�һ����䲻�����󵽡�
    **             SELECT a, b    FROM tbl WHERE a = 1;
    **             SELECT a, b, c FROM tbl WHERE a = 1;
    **
    **
    */
<<<<<<< HEAD
    int nEq;                      /* ����ʹ�������ĵ�ֵ���ʽ�ĸ���*/
    int bInEst = 0;               /* ������� x IN (SELECT...),����Ϊtrue*/
    int nInMul = 1;               /* ����in�Ӿ� */
    double rangeDiv = (double)1;  /* ���Ƽ��������ռ� */
    int nBound = 0;               /* ������Ҫɨ��ı� */
    int bSort = !!pOrderBy;       /* ����Ҫ�ⲿ����ʱΪ�� */
    int bDist = !!pDistinct;      /* ����������distinctʱΪ�� */
    int bLookup = 0;              /* �����Ǹ�������Ϊ�� */
    WhereTerm *pTerm;             /* һ��WHERE�Ӿ� */
#ifdef SQLITE_ENABLE_STAT3
    WhereTerm *pFirstTerm = 0;    /* ��һ����ѯƥ�������*/
#endif

    /* Determine the values of nEq and nInMul 
    ����nEq��nInMulֵ
    */
=======
    int nEq;                      /* Number of == or IN terms matching index ƥ��������==��IN terms��Ŀ */
    int bInEst = 0;               /* True if "x IN (SELECT...)" seen �������"x IN (SELECT...)"��ΪTRUE */
    int nInMul = 1;               /* Number of distinct equalities to lookup ���ҵ�DISTINCT��ʽ����Ŀ */
    double rangeDiv = (double)1;  /* Estimated reduction in search space �����������ռ��ϵļ����� */
    int nBound = 0;               /* Number of range constraints seen ���ֵķ�ΧԼ����Ŀ */
    int bSort = !!pOrderBy;       /* True if external sort required �����Ҫ�ⲿ��ѯ��ΪTRUE */
    int bDist = !!pDistinct;      /* True if index cannot help with DISTINCT ���������DISTINCTû�а�������ΪTRUE */
    int bLookup = 0;              /* True if not a covering index �������һ������������ΪTRUE */
    WhereTerm *pTerm;             /* A single term of the WHERE clause WHERE�Ӿ��һ��������term */
#ifdef SQLITE_ENABLE_STAT3
    WhereTerm *pFirstTerm = 0;    /* First term matching the index ƥ�������ĵ�һ��term */
#endif

    /* Determine the values of nEq and nInMul ȷ��nEq��nInMul��ֵ  */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    for(nEq=0; nEq<pProbe->nColumn; nEq++){
      int j = pProbe->aiColumn[nEq];
      pTerm = findTerm(pWC, iCur, j, notReady, eqTermMask, pIdx);
      if( pTerm==0 ) break;
      wsFlags |= (WHERE_COLUMN_EQ|WHERE_ROWID_EQ);
      testcase( pTerm->pWC!=pWC );
      if( pTerm->eOperator & WO_IN ){
        Expr *pExpr = pTerm->pExpr;
        wsFlags |= WHERE_COLUMN_IN;
        if( ExprHasProperty(pExpr, EP_xIsSelect) ){
          /* "x IN (SELECT ...)":  Assume the SELECT returns 25 rows "x IN (SELECT ...)": �ٶ�SELECT����25�� */
          nInMul *= 25;
          bInEst = 1;
        }else if( ALWAYS(pExpr->x.pList && pExpr->x.pList->nExpr) ){
          /* "x IN (value, value, ...)" */
          nInMul *= pExpr->x.pList->nExpr;
        }
      }else if( pTerm->eOperator & WO_ISNULL ){
        wsFlags |= WHERE_COLUMN_NULL;
      }
#ifdef SQLITE_ENABLE_STAT3
      if( nEq==0 && pProbe->aSample ) pFirstTerm = pTerm;
#endif
      used |= pTerm->prereqRight;
    }
 
    /* If the index being considered is UNIQUE, and there is an equality 
    ** constraint for all columns in the index, then this search will find
    ** at most a single row. In this case set the WHERE_UNIQUE flag to 
    ** indicate this to the caller.
<<<<<<< HEAD
    **�����Ψһ����,��һ�����Լ��������������,
    ��ô���������෢�ֵ��С�
    ���������������WHERE_UNIQUE��־����ʾ���������
    ** Otherwise, if the search may find more than one row, test to see if
    ** there is a range constraint on indexed column (nEq+1) that can be 
    ** optimized using the index. 
  ����,����������ܻᳬ��һ��,
  �����Ƿ���һϵ������������(nEq + 1),����ʹ�������������Ż���
=======
    **
    ** ���������UNIQUE���������Ҷ����������е������ж���һ����ʽԼ������ô����������ཫ����һ����������
    ** ���������������WHERE_UNIQUE��־��������ָ���������
    **
    ** Otherwise, if the search may find more than one row, test to see if
    ** there is a range constraint on indexed column (nEq+1) that can be 
    ** optimized using the index. 
    **
    ** ���������ѯ���ܲ��ҵ��Ľ����ֻһ�У������Ƿ�������������һ����Χ��������ʹ�����������Ż���
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    //����nBoundֵ
    if( nEq==pProbe->nColumn && pProbe->onError!=OE_None ){
      testcase( wsFlags & WHERE_COLUMN_IN );
      testcase( wsFlags & WHERE_COLUMN_NULL );
      if( (wsFlags & (WHERE_COLUMN_IN|WHERE_COLUMN_NULL))==0 ){
        wsFlags |= WHERE_UNIQUE;
      }
    }else if( pProbe->bUnordered==0 ){
      int j = (nEq==pProbe->nColumn ? -1 : pProbe->aiColumn[nEq]);
      if( findTerm(pWC, iCur, j, notReady, WO_LT|WO_LE|WO_GT|WO_GE, pIdx) ){
        WhereTerm *pTop = findTerm(pWC, iCur, j, notReady, WO_LT|WO_LE, pIdx);
        WhereTerm *pBtm = findTerm(pWC, iCur, j, notReady, WO_GT|WO_GE, pIdx);
        //���Ʒ�Χ�����Ĵ���
        whereRangeScanEst(pParse, pProbe, nEq, pBtm, pTop, &rangeDiv);
        if( pTop ){
          nBound = 1;
          wsFlags |= WHERE_TOP_LIMIT;
          used |= pTop->prereqRight;
          testcase( pTop->pWC!=pWC );
        }
        if( pBtm ){
          nBound++;
          wsFlags |= WHERE_BTM_LIMIT;
          used |= pBtm->prereqRight;
          testcase( pBtm->pWC!=pWC );
        }
        wsFlags |= (WHERE_COLUMN_RANGE|WHERE_ROWID_RANGE);
      }
    }

    /* If there is an ORDER BY clause and the index being considered will
    ** naturally scan rows in the required order, set the appropriate flags
    ** in wsFlags. Otherwise, if there is an ORDER BY clause but the index
    ** will scan rows in a different order, set the bSort variable.  
<<<<<<< HEAD
    �����һ��ORDER BY�Ӿ���������ڿ�����Ȼ��ɨ���������˳��,
    ��wsFlags������Ӧ�ı�־��
    ����,�����һ��ORDER BY�Ӿ䵫��������ɨ����˳��ͬ,����bSort������
=======
    **
    ** �����һ��ORDER BY�Ӿ䲢������������Ӧ��������ɨ���У���wsFlags�������ʵ��ı�־��
    ** ���������һ��ORDER BY�Ӿ䵫��������������������ɨ���У�����bSort����
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( isSortingIndex(
          pParse, pWC->pMaskSet, pProbe, iCur, pOrderBy, nEq, wsFlags, &rev)
    ){
      bSort = 0;
      wsFlags |= WHERE_ROWID_RANGE|WHERE_COLUMN_RANGE|WHERE_ORDERBY;
      wsFlags |= (rev ? WHERE_REVERSE : 0);
    }

    /* If there is a DISTINCT qualifier and this index will scan rows in
    ** order of the DISTINCT expressions, clear bDist and set the appropriate
    ** flags in wsFlags.
<<<<<<< HEAD
    ������޶���DISTINCT,����������ɨ�����ò�ͬ��DISTINCT���ʽ,
    ��ȷwsFlags bDist�������ʵ��ı�־��
     */
=======
    **
    ** �����һ��DISTINCT�޶������������������DISTINCT���ʽ��������ɨ���У�
    ** ���bDist������wsFlags���趨�ʵ��ı�־��
    */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    if( isDistinctIndex(pParse, pWC, pProbe, iCur, pDistinct, nEq)
     && (wsFlags & WHERE_COLUMN_IN)==0
    ){
      bDist = 0;
      wsFlags |= WHERE_ROWID_RANGE|WHERE_COLUMN_RANGE|WHERE_DISTINCT;
    }

    /* If currently calculating the cost of using an index (not the IPK
    ** index), determine if all required column data may be obtained without 
    ** using the main table (i.e. if the index is a covering
    ** index for this query). If it is, set the WHERE_IDX_ONLY flag in
<<<<<<< HEAD
    ** wsFlags. Otherwise, set the bLookup variable to true.  
    //���Ŀǰ�ļ���ʹ������(����IPK����)����,
    ȷ����������������ݿ��Ի�ò�ʹ������(������ò�ѯ��������һ����������)��
    �����,��wsFlags���� WHERE_IDX_ONLY��־��
    ����,����bLookup����Ϊtrue��
    */
    if( pIdx && wsFlags ){
      Bitmask m = pSrc->colUsed;
      int j;
      for(j=0; j<pIdx->nColumn; j++){
=======
    ** wsFlags. Otherwise, set the bLookup variable to true.
    **
    ** ������㵱ǰʹ��һ�������Ĵ���(����IPK����)��
    ** ���������ͨ��ʹ�������������е�����������(Ҳ����˵��������������ѯ������һ����������)��
    ** �������һ��������������wsFlags������WHERE_IDX_ONLY��־��
    ** ���򣬰ѱ���bLookup����ΪTRUE.
    */
    if( pIdx && wsFlags ){ //�����������
      Bitmask m = pSrc->colUsed; //����ʹ����������
      int j; //ѭ��������
      for(j=0; j<pIdx->nColumn; j++){  
	  	//��������ʹ�ø��������У��ж��Ƿ������ж���������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        int x = pIdx->aiColumn[j];
        if( x<BMS-1 ){
          m &= ~(((Bitmask)1)<<x);
        }
      }
      if( m==0 ){	//������ж���������
        wsFlags |= WHERE_IDX_ONLY; //����WHERE_IDX_ONLY����־��һ����������
      }else{
        bLookup = 1; //����һ����������
      }
    }

    /*
    ** Estimate the number of rows of output.  For an "x IN (SELECT...)"
    ** constraint, do not let the estimate exceed half the rows in the table.
<<<<<<< HEAD
    ���Ƶ����������������һ����x(ѡ��)��Լ��,��Ҫ�ù��Ƴ���һ��ı��е��С�
=======
    **
    ** �������ݵ�����������һ��"x IN (SELECT...)"Լ������Ҫ�ù���ֵ���������е�һ�롣
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    nRow = (double)(aiRowEst[nEq] * nInMul);
    if( bInEst && nRow*2>aiRowEst[0] ){
      nRow = aiRowEst[0]/2;
      nInMul = (int)(nRow / aiRowEst[nEq]);
    }

#ifdef SQLITE_ENABLE_STAT3
    /* If the constraint is of the form x=VALUE or x IN (E1,E2,...)
    ** and we do not think that values of x are unique and if histogram
    ** data is available for column x, then it might be possible
    ** to get a better estimate on the number of rows based on
    ** VALUE and how common that value is according to the histogram.
<<<<<<< HEAD
    ������Լ����x = xֵ��(E1,E2,��),���ǲ���Ϊx��ֵ��Ψһ��,
    ���ֱ��ͼ���ݿ���x,��ô�����ܵõ����õĹ��ƻ���ֵ������,
    �Լ�������ֵ�Ǹ���ֱ��ͼ��
=======
    **
    ** ���Լ����x=VALUE or x IN (E1,E2,...)������ʽ
    ** �������ǲ���Ϊx��ֵ��Ψһ�Ĳ����������x����˵ֱ��ͼ�����Ǳ�����
    ** ��ô�����ܻ���VALLUE�������ϻ��һ�����õĹ���ֵ����θ���ֱ��ͼ�õ���ͬ��ֵ��
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( nRow>(double)1 && nEq==1 && pFirstTerm!=0 && aiRowEst[1]>1 ){
      assert( (pFirstTerm->eOperator & (WO_EQ|WO_ISNULL|WO_IN))!=0 );
      if( pFirstTerm->eOperator & (WO_EQ|WO_ISNULL) ){
        testcase( pFirstTerm->eOperator==WO_EQ );
        testcase( pFirstTerm->eOperator==WO_ISNULL );
        whereEqualScanEst(pParse, pProbe, pFirstTerm->pExpr->pRight, &nRow);
      }else if( bInEst==0 ){
        assert( pFirstTerm->eOperator==WO_IN );
        whereInScanEst(pParse, pProbe, pFirstTerm->pExpr->x.pList, &nRow);
      }
    }
#endif /* SQLITE_ENABLE_STAT3 */

    /* Adjust the number of output rows and downward to reflect rows
    ** that are excluded by range constraints.
<<<<<<< HEAD
    ��������кͽ��������ų��ķ�Χ���ơ�
=======
    **
    ** ��������е���Ŀ�������·�ӳͨ����ΧԼ���ܾ����С�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    nRow = nRow/rangeDiv;
    if( nRow<1 ) nRow = 1;

    /* Experiments run on real SQLite databases show that the time needed
    ** to do a binary search to locate a row in a table or index is roughly
    ** log10(N) times the time to move from one row to the next row within
    ** a table or index.  The actual times can vary, with the size of
    ** records being an important factor.  Both moves and searches are
    ** slower with larger records, presumably because fewer records fit
    ** on one page and hence more pages have to be fetched.
<<<<<<< HEAD
    **ʵ��������������SQLite���ݿ���ʾ��
    �����ʱ����һ��������������λ���е�һ�л�����log10(N)��ʱ���һ�е���һ����һ�����������
    ʵ��ʱ����ܻ�������ͬ,��¼�Ĵ�С��һ����Ҫ���ء�
    ��¼�����ƶ���������������,�������Ϊ�ټ�¼�ʺ���һ��ҳ����,��˱����ȡ����ҳ��
=======
    **
    ** ������������ʵ��SQLite������ʾ�ڱ����������һ�����ֲ�������λһ�������ʱ�䣬
    ** �ڱ�������У���һ���ƶ�����һ�е�ʱ�����Ϊlog10(N).
    ** ���ż�¼�����ݳ�Ϊһ����Ҫ���أ�ʵ��ʱ����Ա仯��
    ** ����������ļ�¼���ƶ��Ͳ��Ҷ��Ƚ�����
    ** ��������Ϊ��һ��ҳ����װ��ļ�¼���٣������Ҫ��ȡ�ܶ��ҳ��
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** The ANALYZE command and the sqlite_stat1 and sqlite_stat3 tables do
    ** not give us data on the relative sizes of table and index records.
    ** So this computation assumes table records are about twice as big
    ** as index records
<<<<<<< HEAD
    ���������sqlite_stat1 sqlite_stat3�����������ݱ��������¼����Դ�С��
    ��������ٶ����¼��Լ����������¼
=======
    **
    ** ANALYZE�����sqlite_stat1��sqlite_stat3���ڱ�������ļ�¼��û�и������ṩ��Դ�С��
    ** ����������������¼�����������¼��������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( (wsFlags & WHERE_NOT_FULLSCAN)==0 ){
      /* The cost of a full table scan is a number of move operations equal
      ** to the number of rows in the table.
<<<<<<< HEAD
      **ȫ��ɨ��Ĵ�����һ�������ƶ������൱�ڱ��е�������
=======
      **
      ** һ��ȫ��ɨ��Ĵ�����һ���������ƶ������൱���ڱ��е�����
      **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      ** We add an additional 4x penalty to full table scans.  This causes
      ** the cost function to err on the side of choosing an index over
      ** choosing a full scan.  This 4x full-scan penalty is an arguable
      ** decision and one which we expect to revisit in the future.  But
      ** it seems to be working well enough at the moment.
<<<<<<< HEAD
      �������һ�������4��������ȫ��ɨ�衣
      �⵼�µĴ��ۺ�������ѡ��һ��������ѡ��һ��������ɨ�衣
      ��4��ȫɨ�������һ��������ľ���,����ϣ�����¿���δ����
      ��Ŀǰ�ƺ��������㹻��
=======
      **
      ** ���һ�����ӵ�4x�ͷ�����ȫ��ɨ�衣���������ۺ�������ѡ��һ������������ȫ��ɨ�衣
      ** ���4xȫ��ɨ��ͷ���һ������֤�ľ�������ϣ���ں������ٷ��ʡ�
      ** �����������������ʱ���еĻ�����
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      */
      cost = aiRowEst[0]*4;
    }else{
      log10N = estLog(aiRowEst[0]);
      cost = nRow;
      if( pIdx ){
        if( bLookup ){
          /* For an index lookup followed by a table lookup:
          **    nInMul index searches to find the start of each index range
          **  + nRow steps through the index
          **  + nRow table searches to lookup the table entry using the rowid
<<<<<<< HEAD
          һ�����������ڱ���Һ�
          nInMul����������ÿ��������Χ
          + nRow����ͨ������+ nRow����������ʹ��rowid�ı���Ŀ
=======
          **
          ** ����һ������Һ����������:
          ** + nRow���ͨ������
          ** + nRow���������ʹ��rowid�ұ���Ŀ
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          */
          cost += (nInMul + nRow)*log10N;
        }else{
          /* For a covering index:
          **     nInMul index searches to find the initial entry 
          **   + nRow steps through the index
<<<<<<< HEAD
          һ������������
          nInMul������������������ͨ������+ nRow����
=======
          **
          ** ����һ����������:
          **     nInMul���������������������
          **   + nRow���ͨ������
          **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          */
          cost += nInMul*log10N;
        }
      }else{
        /* For a rowid primary key lookup:
        **    nInMult table searches to find the initial entry for each range
        **  + nRow steps through the table
<<<<<<< HEAD
        rowid�������ң�
        nInMult�������ǳ�ʼ��Ŀͨ����Χ+ nRowÿ������
=======
        **
        ** ����һ��rowid��������:
        **	nInMul�����������ÿ����Χ���������
        **  + nRow���ͨ����
        **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        */
        cost += nInMul*log10N;
      }
    }

    /* Add in the estimated cost of sorting the result.  Actual experimental
    ** measurements of sorting performance in SQLite show that sorting time
    ** adds C*N*log10(N) to the cost, where N is the number of rows to be 
    ** sorted and C is a factor between 1.95 and 4.3.  We will split the
    ** difference and select C of 3.0.
<<<<<<< HEAD
    ����������Ĵ��۹��㡣
    ��SQLite�������ܵ�ʵ��ʵ���������,����ʱ��������C * N * log10(N)�Ĵ���,
    ����N��Ҫ�����������C��һ��������1.95��4.3֮�䡣���ǽ���������ѡ��C 3.0��
=======
    **
    ** ����������Ĺ��Ƴɱ���
    ** ��SQLite���������ܵ�ʵ��ʵ��Ĳ�����������ʱ�����C*N*log10(N)�������У�
    ** ����N����Ҫ�����������C��һ����1.95��4.3֮������ء�
    ** ���ǽ����������ѡ��ֵΪ3.0��C
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( bSort ){
      cost += nRow*estLog(nRow)*3;
    }
    if( bDist ){
      cost += nRow*estLog(nRow)*3;
    }

<<<<<<< HEAD
    /**** Cost of using this index has now been computed ****/
    //ʹ�ø������Ĵ����Ѿ��ǿɼ����
=======
    /**** Cost of using this index has now been computed ���ڼ���ʹ����������Ĵ��� ****/

>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    /* If there are additional constraints on this table that cannot
    ** be used with the current index, but which might lower the number
    ** of output rows, adjust the nRow value accordingly.  This only 
    ** matters if the current index is the least costly, so do not bother
    ** with this step if we already know this index will not be chosen.
    ** Also, never reduce the output row count below 2 using this step.
<<<<<<< HEAD
    **����ж�������������,�������ڵ�ǰ����,
    ������ܻή�����������,��Ӧ�ص���nRowֵ��
    ��ֻ����Ҫ�����ǰ����������,
    ���Բ�Ҫ������һ����������Ѿ�֪������������ᱻѡ�С�
    Ҳ����û�м��������������2ʹ��������衣
=======
    **
    ** �������������и��ӵ�Լ�����ܱ����ڵ�ǰ�����ϣ�
    ** �������Լ�����ܻ��������е���Ŀ����ô�͵�����Ӧ��nRowֵ��
    ** ��ֻ����Ϊ��ǰ��������С���ۣ�����������Ƕ�֪��������������ᱻѡ�У���ô�Ͳ�Ҫ�����һ����
    ** ���⣬ʹ���ⲽ�Ӳ�������������ٵ�����2��
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** It is critical that the notValid mask be used here instead of
    ** the notReady mask.  When computing an "optimal" index, the notReady
    ** mask will only have one bit set - the bit for the current table.
    ** The notValid mask, on the other hand, always has all bits set for
    ** tables that are not in outer loops.  If notReady is used here instead
    ** of notValid, then a optimal index that depends on inner joins loops
    ** might be selected even when there exists an optimal index that has
    ** no such dependency.
<<<<<<< HEAD
    �ؼ���Ҳ��notValid����������ʹ�õ�notReady���롣
    ��Ѱ��һ���������,notReady����Ҳ��ֻ����һ������Ϊ��ǰ��
    notValid����,��һ����,������һЩ���ñ�,������ѭ����
    ���Ҳ��������ʹ��notReady������notValid,
    ��ô����������ȡ������������ѭ���������ܱ�ѡ��ʱ,
    ������һ����ѵ�ָ��,û��������������
    */
    if( nRow>2 && cost<=pCost->rCost ){
      int k;                       /* ѭ��������*/
      int nSkipEq = nEq;           /* =Լ����Ծ*/
      int nSkipRange = nBound;     /* <Լ����Ծ*/
      Bitmask thisTab;             /* ����pSrc */
=======
    **
    ** ʹ����Ч���������滻δ׼���õ������Ǻܽ�Ҫ�ġ�
    ** ������һ��"���ŵ�"������δ׼���õ�����ֻ��һ��λ����Ϊ��ǰ��
    ** ��һ���棬��Ч������������λ����Ϊû���ⲿѭ���ı�
    ** �����δ׼���õ�������������Ч�����룬
    ** ��ôѡ��һ�������ڲ�����ѭ������ѵ�������ʹ������һ��û����������ѵ�����
    */
    if( nRow>2 && cost<=pCost->rCost ){
      int k;                       /* Loop counter ѭ�������� */
      int nSkipEq = nEq;           /* Number of == constraints to skip ��Ҫ������==Լ���� */
      int nSkipRange = nBound;     /* Number of < constraints to skip ��Ҫ������<Լ���� */
      Bitmask thisTab;             /* Bitmap for pSrc ����pSrc��λͼ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

      thisTab = getMask(pWC->pMaskSet, iCur);
      for(pTerm=pWC->a, k=pWC->nTerm; nRow>2 && k; k--, pTerm++){
        if( pTerm->wtFlags & TERM_VIRTUAL ) continue;
        if( (pTerm->prereqAll & notValid)!=thisTab ) continue;
        if( pTerm->eOperator & (WO_EQ|WO_IN|WO_ISNULL) ){
          if( nSkipEq ){
            /* Ignore the first nEq equality matches since the index
<<<<<<< HEAD
            ** has already accounted for these 
            ���Ե�һ��nEq���ƥ��ָ����Ϊ�����Ѿ�ռ����Щ
=======
            ** has already accounted for these
            **
            ** ���Ե�һ��nEq��ʽƥ������������Ѿ�˵������Щ
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
            */
            nSkipEq--;
          }else{
            /* Assume each additional equality match reduces the result
<<<<<<< HEAD
            ** set size by a factor of 10 
            ����ÿ����������ƥ��������С����10��
=======
            ** set size by a factor of 10
            **
            ** ����ÿ�����ӵĵ�ʽƥ��������С����10��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
            */
            nRow /= 10;
          }
        }else if( pTerm->eOperator & (WO_LT|WO_LE|WO_GT|WO_GE) ){
          if( nSkipRange ){
            /* Ignore the first nSkipRange range constraints since the index
<<<<<<< HEAD
            ** has already accounted for these
            ���Ե�һ��nSkipRange���ƥ��ָ����Ϊ�����Ѿ�ռ����Щ
             */
=======
            ** has already accounted for these 
            **
            ** ���Ե�һ��nSkipRange��ΧԼ����������Ѿ�˵������Щ
            */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
            nSkipRange--;
          }else{
            /* Assume each additional range constraint reduces the result
            ** set size by a factor of 3.  Indexed range constraints reduce
            ** the search space by a larger factor: 4.  We make indexed range
            ** more selective intentionally because of the subjective 
            ** observation that indexed range constraints really are more
            ** selective in practice, on average. 
<<<<<<< HEAD
            ����ÿ�������Լ����Χ�����˽������С��3����
            ������ΧԼ�����������ռ�ϴ������4����
            ���ǹ���ʹ������Χ����ѡ����,
            ��Ϊ������ΧԼ�������۹۲�ͨ������ѡ���Ե���ʵ���С�
=======
            **
            ** ����ÿ�����ӵķ�ΧԼ���ѽ����������3����
            ** �������ķ�ΧԼ���������ռ������4����
            ** ���ǹ����ʹ��Χ��������ѡ���ԣ���Ϊƽ�����ԣ�������ΧԼ�������۹۲���ѡ������ĸ�����ѡ���ԡ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
            */
            nRow /= 3;
          }
        }else if( pTerm->eOperator!=WO_NOOP ){
          /* Any other expression lowers the output row count by half �����ı��ʽ���������������һ�� */
          nRow /= 2;
        }
      }
      if( nRow<2 ) nRow = 2;
    }


    WHERETRACE((
      "%s(%s): nEq=%d nInMul=%d rangeDiv=%d bSort=%d bLookup=%d wsFlags=0x%x\n"
      "         notReady=0x%llx log10N=%.1f nRow=%.1f cost=%.1f used=0x%llx\n",
      pSrc->pTab->zName, (pIdx ? pIdx->zName : "ipk"), 
      nEq, nInMul, (int)rangeDiv, bSort, bLookup, wsFlags,
      notReady, log10N, nRow, cost, used
    ));

    /* If this index is the best we have seen so far, then record this
    ** index and its cost in the pCost structure.
<<<<<<< HEAD
    ��������������õ������Ѿ���������Ϊֹ,
    Ȼ����pCost��¼������������Ĵ���
=======
    **
    ** �����������ǵ�ĿǰΪֹ��õģ���ô��pCost���ݽṹ�м�¼������������Ĵ��ۡ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( (!pIdx || wsFlags)
     && (cost<pCost->rCost || (cost<=pCost->rCost && nRow<pCost->plan.nRow))
    ){
      pCost->rCost = cost;
      pCost->used = used;
      pCost->plan.nRow = nRow;
      pCost->plan.wsFlags = (wsFlags&wsFlagMask);
      pCost->plan.nEq = nEq;
      pCost->plan.u.pIdx = pIdx;
    }

    /* If there was an INDEXED BY clause, then only that one index is
    ** considered. 
<<<<<<< HEAD
    �����һ������BY�Ӿ�,��ô����Ϊ��Ψһ������
    */
    if( pSrc->pIndex ) break;

    /* Reset masks for the next index in the loop */
    //����ѭ���е���һ������������
=======
    **
    ** �����һ��INDEXED BY�Ӿ䣬��ôֻ�п���һ��������
    */
    if( pSrc->pIndex ) break;

    /* Reset masks for the next index in the loop Ϊ��ѭ���е���һ�������������������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    wsFlagMask = ~(WHERE_ROWID_EQ|WHERE_ROWID_RANGE);
    eqTermMask = idxEqTermMask;
  }

  /* If there is no ORDER BY clause and the SQLITE_ReverseOrder flag
  ** is set, then reverse the order that the index will be scanned
  ** in. This is used for application testing, to help find cases
  ** where application behaviour depends on the (undefined) order that
  ** SQLite outputs rows in in the absence of an ORDER BY clause.  
<<<<<<< HEAD
  ���û������ORDER BY�Ӿ䵫����SQLITE_ReverseOrder��־,
  Ȼ����������˳�򽫱�ɨ�衣
  ��������Ӧ�ó������,�����ҵ��������,
  ����Ӧ�ó������Ϊȡ����(δ����)��SQLite�����û��һ��order BY�Ӿ䡣
=======
  **
  ** ���û��ORDER BY�Ӿ䲢��������SQLITE_ReverseOrder��־����ô�����ķ���˳�򽫱�ɨ�衣
  ** ��ֻ����Ӧ�ò��ԣ����ڰ��������������--Ӧ�ó������Ϊȡ����SQLite��ȱ��ORDER BY�Ӿ������е�(δ�����)���С�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( !pOrderBy && pParse->db->flags & SQLITE_ReverseOrder ){
    pCost->plan.wsFlags |= WHERE_REVERSE;
  }

  assert( pOrderBy || (pCost->plan.wsFlags&WHERE_ORDERBY)==0 );
  assert( pCost->plan.u.pIdx==0 || (pCost->plan.wsFlags&WHERE_ROWID_EQ)==0 );
  assert( pSrc->pIndex==0 
       || pCost->plan.u.pIdx==0 
       || pCost->plan.u.pIdx==pSrc->pIndex 
  );

  WHERETRACE(("best index is: %s\n", 
    ((pCost->plan.wsFlags & WHERE_NOT_FULLSCAN)==0 ? "none" : 
         pCost->plan.u.pIdx ? pCost->plan.u.pIdx->zName : "ipk")
  ));
  
  bestOrClauseIndex(pParse, pWC, pSrc, notReady, notValid, pOrderBy, pCost);
  bestAutomaticIndex(pParse, pWC, pSrc, notReady, pCost);
  pCost->plan.wsFlags |= eqTermMask;
}

/*
** Find the query plan for accessing table pSrc->pTab. Write the
** best query plan and its cost into the WhereCost object supplied 
** as the last parameter. This function may calculate the cost of
** both real and virtual table scans.
**
** ���ҷ��ʱ�pSrc->pTab�Ĳ�ѯ�ƻ�����WhereCost������д����õĲ�ѯ�ƻ������Ĵ��ۣ�������Ϊ��õĲ������ݸ�bestIndex������
** ����������ܻ�����ɨ��������ɨ��Ĵ���
**
*/
static void bestIndex(
<<<<<<< HEAD
  Parse *pParse,              /* ����������*/
  WhereClause *pWC,           /* WHERE�Ӿ�*/
  struct SrcList_item *pSrc,  /* form��ѯ�Ӿ� */
  Bitmask notReady,           /* ���������������α����� */
  Bitmask notValid,           /* ���������κ���;���α� */
  ExprList *pOrderBy,         /* ORDER BY �Ӿ� */
  WhereCost *pCost            /* ������С�Ĳ�ѯ���� */
=======
  Parse *pParse,              /* The parsing context ���������� */
  WhereClause *pWC,           /* The WHERE clause WHERE�Ӿ� */
  struct SrcList_item *pSrc,  /* The FROM clause term to search ���ڲ��ҵ�FROM�Ӿ�term */
  Bitmask notReady,           /* Mask of cursors not available for indexing ����������Ч���α����� */
  Bitmask notValid,           /* Cursors not available for any purpose �α�����κ��������Ч */
  ExprList *pOrderBy,         /* The ORDER BY clause ORDER BY�Ӿ� */
  WhereCost *pCost            /* Lowest cost query plan ��ʹ��۵Ĳ�ѯ�ƻ� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( IsVirtual(pSrc->pTab) ){ //��������
    sqlite3_index_info *p = 0;
    bestVirtualIndex(pParse, pWC, pSrc, notReady, notValid, pOrderBy, pCost,&p);
    if( p->needToFreeIdxStr ){
      sqlite3_free(p->idxStr);
    }
    sqlite3DbFree(pParse->db, p);	//�ͷſ��ܱ�������һ���ض����ݿ����ӵ��ڴ�
  }else //����������
#endif
  {
    bestBtreeIndex(pParse, pWC, pSrc, notReady, notValid, pOrderBy, 0, pCost);
  }
}

/*
** Disable a term in the WHERE clause.  Except, do not disable the term
** if it controls a LEFT OUTER JOIN and it did not originate in the ON
** or USING clause of that join.
<<<<<<< HEAD
**����һ��������WHERE�Ӿ��С�
����,��Ҫ���ô����������������,
����������Դ��ON��USING�Ӿ�ʹ�á�
=======
**
** ��WHERE�Ӿ��н�ֹһ��term.���������һ��LEFT OUTER JOIN����������Դ���Ǹ����ӵ�ON��USING�Ӿ�ʱ����ֹterm.
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** Consider the term t2.z='ok' in the following queries:
**�����������t2.z=��ok��������Ĳ�ѯ��
**   (1)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x WHERE t2.z='ok'
**   (2)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x AND t2.z='ok'
**   (3)  SELECT * FROM t1, t2 WHERE t1.a=t2.x AND t2.z='ok'
**
** The t2.z='ok' is disabled in the in (2) because it originates
** in the ON clause.  The term is disabled in (3) because it is not part
** of a LEFT OUTER JOIN.  In (1), the term is not disabled.
<<<<<<< HEAD
**t2.z='ok'�Ǵ�����ڣ�2���У���Ϊ�����γ��ֲ���on�Ӿ��С�
t2.z='ok'�Ǵ�����ڣ�3���У���Ϊ�����������ӵ�һ���֡�
�ڣ�1���С�������ȷ��
** IMPLEMENTATION-OF: R-24597-58655 No tests are done for terms that are
** completely satisfied by indices.
**�������Ե�������ȫ����ָ��
=======
**
** ���������ѯ�е�t2.z='ok'
**   (1)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x WHERE t2.z='ok'
**   (2)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x AND t2.z='ok'
**   (3)  SELECT * FROM t1, t2 WHERE t1.a=t2.x AND t2.z='ok'
** ��Ϊt2.z='ok'��Դ��ON�Ӿ䣬������(2)�н���t2.z='ok'.
** ��Ϊt2.z='ok'����LEFT OUTER JOIN��һ���֣�������(3)�н���t2.z='ok'.
** ��(1)�У�termδ����ֹ��
** 
** IMPLEMENTATION-OF: R-24597-58655 No tests are done for terms that are
** completely satisfied by indices. 
**
** �������Ե�terms����ȫ����ʹ�������ġ�
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** Disabling a term causes that term to not be tested in the inner loop
** of the join.  Disabling is an optimization.  When terms are satisfied
** by indices, we disable them to prevent redundant tests in the inner
** loop.  We would get the correct results if nothing were ever disabled,
** but joins might run a little slower.  The trick is to disable as much
** as we can without disabling too much.  If we disabled in (1), we'd get
** the wrong answer.  See ticket #813.
<<<<<<< HEAD
����һ������ʹ����ʲ���������ѭ�����ԡ�������һ���Ż���
��������������,���ǽ�������,��ֹ�ڲ�ѭ��������ԡ�
���ǿ��Եõ���ȷ�Ľ�����û�б�����,�����ӿ��������е�����
�����ǽ���һ�����ǿ���û�н���̫�ࡣ
���������(1)����,���ǻ�õ�����Ĵ𰸣����813��
=======
**
** ��ֹһ��term�������������е��ڲ�ѭ���в�����term.
** ��ֹ��һ���Ż����ԡ���terms����ʹ������ʱ�����ǽ���������ֹ���ڲ�ѭ���еĶ�����ԡ�
** ���û����Զ�����õ�term�����ǻ�õ���ȷ�Ľṹ���������ӿ��ܻ����еĽ�����
** �����ǽ������ǿ��Խ��õģ�����Ҫ����̫�ࡣ���������(1)�н����ˣ���ô���ǿ��ܻ�õ�����Ľ����
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
*/
static void disableTerm(WhereLevel *pLevel, WhereTerm *pTerm){
  if( pTerm
      && (pTerm->wtFlags & TERM_CODED)==0
      && (pLevel->iLeftJoin==0 || ExprHasProperty(pTerm->pExpr, EP_FromJoin))
  ){
    pTerm->wtFlags |= TERM_CODED;
    if( pTerm->iParent>=0 ){
      WhereTerm *pOther = &pTerm->pWC->a[pTerm->iParent];
      if( (--pOther->nChild)==0 ){
        disableTerm(pLevel, pOther);
      }
    }
  }
}

/*
** Code an OP_Affinity opcode to apply the column affinity string zAff
** to the n registers starting at base. 
<<<<<<< HEAD
**����һ��OP_Affinity��������ַ���zAff�Ĵ����ӻ�����ʼ�������й�ϵ
** As an optimization, SQLITE_AFF_NONE entries (which are no-ops) at the
** beginning and end of zAff are ignored.  If all entries in zAff are
** SQLITE_AFF_NONE, then no code gets generated.
**��Ϊһ���Ż�,SQLITE_AFF_NONE��Ŀ(�޲���)�Ŀ�ʼ�ͽ���zAff�����ԡ�
���������ĿzAff ״̬��SQLITE_AFF_NONE,��ôû�д������ɡ�
** This routine makes its own copy of zAff so that the caller is free
** to modify zAff after this routine returns.
�������zAff�Լ��ĸ���,
�Ա�����߿������ɵ��޸ĺ�zAff������̷��ء�
=======
**
** ��дһ��OP_Affinity������Ӧ���ڰ����׺�string���͵�zAffӳ�䵽�ӻ�ַ��n�ļĴ����ϡ�
**
** As an optimization, SQLITE_AFF_NONE entries (which are no-ops) at the
** beginning and end of zAff are ignored.  If all entries in zAff are
** SQLITE_AFF_NONE, then no code gets generated.
**
** ��Ϊһ���Ż���������zAff�Ŀ�ʼ�ͽ�β�е�SQLITE_AFF_NONE��Ŀ��
** �����zAff������Ŀ����SQLITE_AFF_NONE����ô�������ɴ��롣
**
** This routine makes its own copy of zAff so that the caller is free
** to modify zAff after this routine returns.
**
** ������������Լ���zAff�Ա���������򷵻غ�����߿��������޸�zAff��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
*/
static void codeApplyAffinity(Parse *pParse, int base, int n, char *zAff){
  Vdbe *v = pParse->pVdbe;
  if( zAff==0 ){
    assert( pParse->db->mallocFailed );
    return;
  }
  assert( v!=0 );

  /* Adjust base and n to skip over SQLITE_AFF_NONE entries at the beginning
  ** and end of the affinity string.
<<<<<<< HEAD
  ����������n,����SQLITE_AFF_NONE��Ŀ�ڿ�ʼ�ͽ�����
=======
  **
  ** ������ַ��n������string�׺��ԵĿ�ʼ�ͽ�β����SQLITE_AFF_NONE��Ŀ
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  while( n>0 && zAff[0]==SQLITE_AFF_NONE ){
    n--;
    base++;
    zAff++;
  }
  while( n>1 && zAff[n-1]==SQLITE_AFF_NONE ){
    n--;
  }

<<<<<<< HEAD
  /* Code the OP_Affinity opcode if there is anything left to do. 
  ����OP_Affinity������,�����ʲô��Ҫȥ��
  */
=======
  /* Code the OP_Affinity opcode if there is anything left to do. ��дOP_Affinity������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  if( n>0 ){
    sqlite3VdbeAddOp2(v, OP_Affinity, base, n);
    sqlite3VdbeChangeP4(v, -1, zAff, n);
    sqlite3ExprCacheAffinityChange(pParse, base, n);
  }
}


/*
** Generate code for a single equality term of the WHERE clause.  An equality
** term can be either X=expr or X IN (...).   pTerm is the term to be 
** coded.
<<<<<<< HEAD
**���ɴ���Ϊһ����ȵ�WHERE�Ӿ䡣һ����ȵ����������X = expr��X IN(��)��pTerm�Ǳ������
** The current value for the constraint is left in register iReg.
**��ǰֵ��Լ��������iReg�Ĵ�����
** For a constraint of the form X=expr, the expression is evaluated and its
** result is left on the stack.  For constraints of the form X IN (...)
** this routine sets up a loop that will iterate over all values of X.
X = exprԼ������ʽ,������ʽ,�������ڶ�ջ�ϡ�
X IN (...)��Լ������ʽΪX�����������һ��ѭ���������е�Xֵ
*/
static int codeEqualityTerm(
  Parse *pParse,      /* ����������t */
  WhereTerm *pTerm,   /* TWHERE�Ӿ䱻���� */
  WhereLevel *pLevel, /* ��ǰ�������еģ�����Ӿ�*/
  int iTarget         /* ��ͼ�ѽ���뿪����Ĵ��� */
){
  Expr *pX = pTerm->pExpr;
  Vdbe *v = pParse->pVdbe;
  int iReg;                  /* �Ĵ�����Ž�� */
=======
**
** Ϊһ��WHERE�Ӿ�ĵ�ʽterm���ɴ��롣һ����ʽterm������X=expr��X IN (...).
** pTerm����Ҫ�����term.
**
** The current value for the constraint is left in register iReg.
**
** �ڼĴ���iReg�м�¼Լ���ĵ�ǰֵ
**
** For a constraint of the form X=expr, the expression is evaluated and its
** result is left on the stack.  For constraints of the form X IN (...)
** this routine sets up a loop that will iterate over all values of X.
**
** ����һ����ʽΪX=expr��Լ����������ʽ�����ڶ�ջ�д洢���Ľ����
** ����X IN (...)��ʽ��Լ����������򴴽�һ��ѭ��������X������ֵ��
*/
static int codeEqualityTerm(
  Parse *pParse,      /* The parsing context ���������� */
  WhereTerm *pTerm,   /* The term of the WHERE clause to be coded ��Ҫ�����WHERE�Ӿ��term */
  WhereLevel *pLevel, /* When level of the FROM clause we are working on FROM�Ӿ�ĵȼ� */
  int iTarget         /* Attempt to leave results in this register ����������Ĵ����д洢��� */
){
  Expr *pX = pTerm->pExpr;
  Vdbe *v = pParse->pVdbe;
  int iReg;                  /* Register holding results �Ĵ��������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

  assert( iTarget>0 );
  if( pX->op==TK_EQ ){
    iReg = sqlite3ExprCodeTarget(pParse, pX->pRight, iTarget);
  }else if( pX->op==TK_ISNULL ){
    iReg = iTarget;
    sqlite3VdbeAddOp2(v, OP_Null, 0, iReg);
#ifndef SQLITE_OMIT_SUBQUERY
  }else{
    int eType;
    int iTab;
    struct InLoop *pIn;

    assert( pX->op==TK_IN );
    iReg = iTarget;
    eType = sqlite3FindInIndex(pParse, pX, 0);
    iTab = pX->iTable;
    sqlite3VdbeAddOp2(v, OP_Rewind, iTab, 0);
    assert( pLevel->plan.wsFlags & WHERE_IN_ABLE );
    if( pLevel->u.in.nIn==0 ){
      pLevel->addrNxt = sqlite3VdbeMakeLabel(v);
    }
    pLevel->u.in.nIn++;
    pLevel->u.in.aInLoop =
       sqlite3DbReallocOrFree(pParse->db, pLevel->u.in.aInLoop,
                              sizeof(pLevel->u.in.aInLoop[0])*pLevel->u.in.nIn);
    pIn = pLevel->u.in.aInLoop;
    if( pIn ){
      pIn += pLevel->u.in.nIn - 1;
      pIn->iCur = iTab;
      if( eType==IN_INDEX_ROWID ){
        pIn->addrInTop = sqlite3VdbeAddOp2(v, OP_Rowid, iTab, iReg);
      }else{
        pIn->addrInTop = sqlite3VdbeAddOp3(v, OP_Column, iTab, 0, iReg);
      }
      sqlite3VdbeAddOp1(v, OP_IsNull, iReg);
    }else{
      pLevel->u.in.nIn = 0;
    }
#endif
  }
  disableTerm(pLevel, pTerm);
  return iReg;
}

/*
** Generate code that will evaluate all == and IN constraints for an
** index.
<<<<<<< HEAD
**���ɵĴ��뽫��������= =��IIN ����һ������
=======
**
** Ϊһ����������һ����������==��INԼ���Ĵ��롣
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** For example, consider table t1(a,b,c,d,e,f) with index i1(a,b,c).
** Suppose the WHERE clause is this:  a==5 AND b IN (1,2,3) AND c>5 AND c<10
** The index has as many as three equality constraints, but in this
** example, the third "c" value is an inequality.  So only two 
** constraints are coded.  This routine will generate code to evaluate
** a==5 and b IN (1,2,3).  The current values for a and b will be stored
** in consecutive registers and the index of the first register is returned.
<<<<<<< HEAD
**���磬���Ǵ������� i1(a,b,c)�ı�t1(a,b,c,d,e,f)
����where�Ӿ��ǣ� a==5 AND b IN (1,2,3) AND c>5 AND c<10
���������3����ʽԼ�������������������С���������c����ֵ�ǲ��ȡ�
����ֻ��������ʽԼ�������롣
��ͬ�������ڱ���a==5 and b IN (1,2,3)Ŀǰֵ����a��b�洢�������ļĴ����У�
���ҵ�һ������ֵ�����ء�
=======
**
** ���磬����һ��������i1(a,b,c)�ı�t1(a,b,c,d,e,f).
** ����WHERE�Ӿ���������:a==5 AND b IN (1,2,3) AND c>5 AND c<10��
** ������3����ʽԼ������������������У�������ֵ"c"��һ������ʽԼ��������ֻ��������Լ�������롣
** ����������ɼ���a==5��b IN (1,2,3)�Ĵ��롣��ǰa��b��ֵ�����洢�������ļĴ����в��ҷ��ص�һ���Ĵ�����ָ�롣
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** In the example above nEq==2.  But this subroutine works for any value
** of nEq including 0.  If nEq==0, this routine is nearly a no-op.
** The only thing it does is allocate the pLevel->iMem memory cell and
** compute the affinity string.
<<<<<<< HEAD
**������ʾ����nEq==2.��������ӳ����ʺ����κ�nEqֵ������0��
���nEq==0��������򼸺����޲�����
����Ψһ�Ĳ������Ƿ���pLevel - > iMem�Ĵ洢��Ԫ�ͼ���������ַ���
=======
**
** �������������nEq==2.������ӳ���ΪnEq����ֵ����(����0).���nEq==0,�����������һ���ղ�����
** ��ֻ��ΪpLevel->iMem����洢��Ԫ�ͼ����׺��ַ���
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** This routine always allocates at least one memory cell and returns
** the index of that memory cell. The code that
** calls this routine will use that memory cell to store the termination
** key value of the loop.  If one or more IN operators appear, then
** this routine allocates an additional nEq memory cells for internal
** use.
<<<<<<< HEAD
**���ʾ���������ٷ���һ���洢��Ԫ�����������Ĵ洢��Ԫ��
��������������������̵Ĵ��뽫ʹ�øô洢��Ԫ�洢��ֹѭ���ļ�ֵ��
���һ������IN ��������,��ô������̷������nEq�洢��Ԫ���ڲ�ʹ�á�
=======
**
** ����������Ƿ�������һ���ڴ浥Ԫ�ͷ����ڴ浥Ԫ�ĵ�ַ��
** ��������������ʹ���ڴ浥Ԫ���洢ѭ���ն˵Ĺؼ�ֵ��
** �������һ������IN����������ô����������һ�����ӵ�nEq�ڴ浥Ԫ���ڲ�ʹ�á�
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** Before returning, *pzAff is set to point to a buffer containing a
** copy of the column affinity string of the index allocated using
** sqlite3DbMalloc(). Except, entries in the copy of the string associated
** with equality constraints that use NONE affinity are set to
** SQLITE_AFF_NONE. This is to deal with SQL such as the following:
**�ڷ���ǰ��* pzAff����Ϊָ�򻺳�������������
�й������ַ����ĸ���������ʹ��sqlite3DbMalloc()��
������Ŀ��������ַ����ĸ������е�ʽԼ��������ʹ�ý�SQLITE_AFF_NONE��ʾû������
����SQL����������:
**   CREATE TABLE t1(a TEXT PRIMARY KEY, b);
**   SELECT ... FROM t1 AS t2, t1 WHERE t1.a = t2.b;
**
** In the example above, the index on t1(a) has TEXT affinity. But since
** the right hand side of the equality constraint (t2.b) has NONE affinity,
** no conversion should be attempted before using a t2.b value as part of
** a key to search the index. Hence the first byte in the returned affinity
** string in this example would be set to SQLITE_AFF_NONE.
<<<<<<< HEAD
������������С���t1(a)��һ��TEXT������������Ϊ��ʽ�ұ�t2.bû�й�����
��ʹ��t2.bֵ��Ϊ���������Ĺؼ���һ���֣�û��ת��Ӧ�ó��ԡ�
�����������ӣ������ַ����е�һ���ֽڽ�������ΪSQLITE_AFF_NONE��
*/
static int codeAllEqualityTerms(
  Parse *pParse,        /* ���������� */
  WhereLevel *pLevel,   /*����FROMǶ��ѭ�� */
  WhereClause *pWC,     /* where�Ӿ�*/
  Bitmask notReady,     /* ����FROMû�б��� */
  int nExtraReg,        /* �����������ļĴ��� */
  char **pzAff          /*����Ϊָ��������ַ��� */
){
  int nEq = pLevel->plan.nEq;   /* ���� ����== ���� INԼ�����Ӿ� */
  Vdbe *v = pParse->pVdbe;      /*����vm */
  Index *pIdx;                  /* �������������ѭ��*/
  int iCur = pLevel->iTabCur;   /* ����α� */
  WhereTerm *pTerm;             /* һ���򵥵�Լ����ѯ */
  int j;                        /* ѭ�������� */
  int regBase;                  /* ��ַ�Ĵ���*/
  int nReg;                     /* ����Ĵ�����Ŀ */
  char *zAff;                   /* ���ع����ַ��� */

  /* This module is only called on query plans that use an index. 
  ���ģ��ֻ������ʹ�������Ĳ�ѯ�ƻ���
  */
=======
**
** �ڷ���ǰ������*pzAffָ��һ��ʹ��sqlite3DbMalloc()����İ������������׺��ַ����ĸ����Ļ�������
** �����ʽԼ����ص�string�ĸ����е���Ŀʹ�����׺��Ա�����ΪSQLITE_AFF_NONE.
** ������������SQL:
**   CREATE TABLE t1(a TEXT PRIMARY KEY, b);
**   SELECT ... FROM t1 AS t2, t1 WHERE t1.a = t2.b;
** ������������У���t1(a)�ϵ�������TEXT�׺��ԡ��������ڵ�ʽԼ�����ұ�(t2.b)û���׺���,
** ��ʹ��һ��t2.b��λһ�����������ؼ���һ����֮ǰ�����Բ��Ե�ʽԼ������ת����
** ��Ϊ����������з��ص��׺��ַ����ĵ�һ���ֽڽ�����ΪSQLITE_AFF_NONE.
*/
static int codeAllEqualityTerms(
  Parse *pParse,        /* Parsing context ���������� */
  WhereLevel *pLevel,   /* Which nested loop of the FROM we are coding ���Ƕ�FROM��Ƕ��ѭ�����б��� */
  WhereClause *pWC,     /* The WHERE clause WHERE�Ӿ� */
  Bitmask notReady,     /* Which parts of FROM have not yet been coded FROM�л�δ����Ĳ��� */
  int nExtraReg,        /* Number of extra registers to allocate ��Ҫ�������ļĴ����� */
  char **pzAff          /* OUT: Set to point to affinity string ����ָ���׺��ַ��� */
){
  int nEq = pLevel->plan.nEq;   /* The number of == or IN constraints to code ������==��INԼ���� */
  Vdbe *v = pParse->pVdbe;      /* The vm under construction ���ڴ�����VM */
  Index *pIdx;                  /* The index being used for this loop �������ѭ�������� */
  int iCur = pLevel->iTabCur;   /* The cursor of the table ���α� */
  WhereTerm *pTerm;             /* A single constraint term һ��������Լ��term */
  int j;                        /* Loop counter ѭ�������� */
  int regBase;                  /* Base register ��ַ�Ĵ��� */
  int nReg;                     /* Number of registers to allocate ���ڷ���ļĴ����� */
  char *zAff;                   /* Affinity string to return ���ص��׺��ַ��� */

  /* This module is only called on query plans that use an index. ���ģ��ֻ���ڲ�ѯ�ƻ�ʹ������ʱ������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  assert( pLevel->plan.wsFlags & WHERE_INDEXED );
  pIdx = pLevel->plan.u.pIdx;

  /* Figure out how many memory cells we will need then allocate them.
<<<<<<< HEAD
  �ҳ�������Ҫ���ٴ洢��ԪȻ���������
=======
  ** ���������Ҫ��������ڴ浥Ԫ
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  regBase = pParse->nMem + 1;
  nReg = pLevel->plan.nEq + nExtraReg;
  pParse->nMem += nReg;

  zAff = sqlite3DbStrDup(pParse->db, sqlite3IndexAffinityStr(v, pIdx));
  if( !zAff ){
    pParse->db->mallocFailed = 1;
  }

  /* Evaluate the equality constraints
<<<<<<< HEAD
  ����ƽ��Լ��
=======
  ** �����ʽԼ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  assert( pIdx->nColumn>=nEq );
  for(j=0; j<nEq; j++){
    int r1;
    int k = pIdx->aiColumn[j];
    pTerm = findTerm(pWC, iCur, k, notReady, pLevel->plan.wsFlags, pIdx);
    if( pTerm==0 ) break;
    /* The following true for indices with redundant columns. 
<<<<<<< HEAD
    ���������������������С�
=======
    ** Ex: CREATE INDEX i1 ON t1(a,b,a); SELECT * FROM t1 WHERE a=0 AND b=0; 
    **
    ** ������������е�������˵����������ȷ�ġ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** Ex: CREATE INDEX i1 ON t1(a,b,a); SELECT * FROM t1 WHERE a=0 AND b=0; 
    */
    testcase( (pTerm->wtFlags & TERM_CODED)!=0 );
    testcase( pTerm->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
    r1 = codeEqualityTerm(pParse, pTerm, pLevel, regBase+j);
    if( r1!=regBase+j ){
      if( nReg==1 ){
        sqlite3ReleaseTempReg(pParse, regBase);
        regBase = r1;
      }else{
        sqlite3VdbeAddOp2(v, OP_SCopy, r1, regBase+j);
      }
    }
    testcase( pTerm->eOperator & WO_ISNULL );
    testcase( pTerm->eOperator & WO_IN );
    if( (pTerm->eOperator & (WO_ISNULL|WO_IN))==0 ){
      Expr *pRight = pTerm->pExpr->pRight;
      sqlite3ExprCodeIsNullJump(v, pRight, regBase+j, pLevel->addrBrk);
      if( zAff ){
        if( sqlite3CompareAffinity(pRight, zAff[j])==SQLITE_AFF_NONE ){
          zAff[j] = SQLITE_AFF_NONE;
        }
        if( sqlite3ExprNeedsNoAffinityChange(pRight, zAff[j]) ){
          zAff[j] = SQLITE_AFF_NONE;
        }
      }
    }
  }
  *pzAff = zAff;
  return regBase;
}

#ifndef SQLITE_OMIT_EXPLAIN
/*
** This routine is a helper for explainIndexRange() below
<<<<<<< HEAD
**����������� explainIndexRange()�İ���
=======
**
** ���������explainIndexRange()
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** pStr holds the text of an expression that we are building up one term
** at a time.  This routine adds a new term to the end of the expression.
** Terms are separated by AND so add the "AND" text for second and subsequent
** terms only.
<<<<<<< HEAD
pStr����һ�����ʽ���ı�,�������ڽ���һ����ѯ��
����������һ��������ı�
��ѯ�Ǳ�AND����,������ӡ�AND���ı������ڶ������Ĳ�ѯ
*/
static void explainAppendTerm(
  StrAccum *pStr,             /* �����ı���� */
  int iTerm,                  /*������̵���������0��ʼ */
  const char *zColumn,        /* ���� */
  const char *zOp             /* ������ */
=======
**
** ������ÿ�ι���һ��termʱ����pStr������ʽ�����ݡ���������ڱ��ʽ���������һ���µ�term.
** Terms�Ǹ���AND�ָ��ģ�����ֻΪ�ڶ���������terms���һ��"AND".
*/
static void explainAppendTerm(
  StrAccum *pStr,             /* The text expression being built �������ı����ʽ */
  int iTerm,                  /* Index of this term.  First is zero ���term���±꣬��һ����0 */
  const char *zColumn,        /* Name of the column ���� */
  const char *zOp             /* Name of the operator �������� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  if( iTerm ) sqlite3StrAccumAppend(pStr, " AND ", 5);
  sqlite3StrAccumAppend(pStr, zColumn, -1);
  sqlite3StrAccumAppend(pStr, zOp, 1);
  sqlite3StrAccumAppend(pStr, "?", 1);
}

/*
** Argument pLevel describes a strategy for scanning table pTab. This 
** function returns a pointer to a string buffer containing a description
** of the subset of table rows scanned by the strategy in the form of an
** SQL expression. Or, if all rows are scanned, NULL is returned.
<<<<<<< HEAD
**����ɨ���pTab pLevel����ս�ԡ�
�����������һ��ָ��һ���ַ���������,
���а���һ���������Ӽ��ı���ɨ����Ե���ʽһ��SQL���ʽ
=======
**
** ����pLevel����һ��ɨ���pTab�Ĳ��ԡ������������һ��ָ�룬ָ��һ���ַ�����������
** �����ַ�������������һ��ͨ����һ��SQL���ʽ��ʽ�Ĳ���ɨ�����е��Ӽ���������
** ���ߣ����ɨ�������У�����NULL.
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** For example, if the query:
**
**   SELECT * FROM t1 WHERE a=1 AND b>2;
**
** is run and there is an index on (a, b), then this function returns a
** string similar to:
**
**   "a=? AND b>?"
**
** The returned pointer points to memory obtained from sqlite3DbMalloc().
** It is the responsibility of the caller to free the buffer when it is
** no longer required.
<<<<<<< HEAD
���ص�ָ��ָ����ڴ��sqlite3DbMalloc()��á�
�����ߵ����������ɻ�����ʱ,�������Ǳ����
=======
**
** ���磬�����ѯ:
**   SELECT * FROM t1 WHERE a=1 AND b>2;
** ���в�����һ����(a, b)�ϵ���������ô�����������һ������"a=? AND b>?"���ַ�����
** ���ص�ָ��ָ���sqlite3DbMalloc()�õ����ڴ档�����ߵ����ξ��ǵ������ٱ�����ʱ���ͷŻ�������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
*/
static char *explainIndexRange(sqlite3 *db, WhereLevel *pLevel, Table *pTab){
  WherePlan *pPlan = &pLevel->plan;
  Index *pIndex = pPlan->u.pIdx;
  int nEq = pPlan->nEq;
  int i, j;
  Column *aCol = pTab->aCol;
  int *aiColumn = pIndex->aiColumn;
  StrAccum txt;

  if( nEq==0 && (pPlan->wsFlags & (WHERE_BTM_LIMIT|WHERE_TOP_LIMIT))==0 ){
    return 0;
  }
  sqlite3StrAccumInit(&txt, 0, 0, SQLITE_MAX_LENGTH);
  txt.db = db;
  sqlite3StrAccumAppend(&txt, " (", 2);
  for(i=0; i<nEq; i++){
    explainAppendTerm(&txt, i, aCol[aiColumn[i]].zName, "=");
  }

  j = i;
  if( pPlan->wsFlags&WHERE_BTM_LIMIT ){
    char *z = (j==pIndex->nColumn ) ? "rowid" : aCol[aiColumn[j]].zName;
    explainAppendTerm(&txt, i++, z, ">");
  }
  if( pPlan->wsFlags&WHERE_TOP_LIMIT ){
    char *z = (j==pIndex->nColumn ) ? "rowid" : aCol[aiColumn[j]].zName;
    explainAppendTerm(&txt, i, z, "<");
  }
  sqlite3StrAccumAppend(&txt, ")", 1);
  return sqlite3StrAccumFinish(&txt);
}

/*
** This function is a no-op unless currently processing an EXPLAIN QUERY PLAN
** command. If the query being compiled is an EXPLAIN QUERY PLAN, a single
** record is added to the output to describe the table scan strategy in 
** pLevel.
<<<<<<< HEAD
����������޲���,����Ŀǰ������Ͳ�ѯ�ƻ����
�����ѯ������һ�����Ͳ�ѯ�ƻ�,
������¼��ӵ��������������ɨ�����
*/
static void explainOneScan(
  Parse *pParse,                  /* ����������*/
  SrcList *pTabList,              /* ���ѭ���Ǳ����ѭ��*/
  WhereLevel *pLevel,             /* ɨ�貢дOP_Explain������*/
  int iLevel,                     /* ֵ�ü�¼��� */
  int iFrom,                      /* ֵ��form����� */
  u16 wctrlFlags                  /* sqlite3WhereBegin() �ı��*/
=======
**
** ���������һ���ղ��������ǵ�ǰִ��һ��EXPLAIN QUERY PLAN���
** �����ʼ����Ĳ�ѯ��һ��EXPLAIN QUERY PLAN������ǻ����һ����¼��������pLevel�еı�ɨ����ԡ�
*/
static void explainOneScan(
  Parse *pParse,                  /* Parse context ���������� */
  SrcList *pTabList,              /* Table list this loop refers to ���ѭ�����õı��б� */
  WhereLevel *pLevel,             /* Scan to write OP_Explain opcode for ɨ��д���OP_Explain������ */
  int iLevel,                     /* Value for "level" column of output �����"level"�е�ֵ */
  int iFrom,                      /* Value for "from" column of output �����"from"�е�ֵ */
  u16 wctrlFlags                  /* Flags passed to sqlite3WhereBegin() ����sqlite3WhereBegin()�ı�־ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
){
  if( pParse->explain==2 ){
    u32 flags = pLevel->plan.wsFlags;
    struct SrcList_item *pItem = &pTabList->a[pLevel->iFrom];
<<<<<<< HEAD
    Vdbe *v = pParse->pVdbe;      /* ����VM  */
    sqlite3 *db = pParse->db;     /* ���ݿ��� */
    char *zMsg;                   /* �ı���ӵ�EQP��� */
    sqlite3_int64 nRow;           /* Ԥ�ڵķ���ͨ��ɨ������� */
    int iId = pParse->iSelectId;  /* Select id (left-most output column) */
    int isSearch;                 /* True for a SEARCH. False for SCAN. */
=======
    Vdbe *v = pParse->pVdbe;      /* VM being constructed ����VM */
    sqlite3 *db = pParse->db;     /* Database handle ���ݿ��� */
    char *zMsg;                   /* Text to add to EQP output ��ӵ�EQP������ı� */
    sqlite3_int64 nRow;           /* Expected number of rows visited by scan ͨ��ɨ����ʵ�Ԥ�ڵ����� */
    int iId = pParse->iSelectId;  /* Select id (left-most output column) ѡ�е�id(����ߵ������) */
    int isSearch;                 /* True for a SEARCH. False for SCAN. ��һ��������ΪTRUE��ɨ����ΪFALSE */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

    if( (flags&WHERE_MULTI_OR) || (wctrlFlags&WHERE_ONETABLE_ONLY) ) return;

    isSearch = (pLevel->plan.nEq>0)
             || (flags&(WHERE_BTM_LIMIT|WHERE_TOP_LIMIT))!=0
             || (wctrlFlags&(WHERE_ORDERBY_MIN|WHERE_ORDERBY_MAX));

    zMsg = sqlite3MPrintf(db, "%s", isSearch?"SEARCH":"SCAN");
    if( pItem->pSelect ){
      zMsg = sqlite3MAppendf(db, zMsg, "%s SUBQUERY %d", zMsg,pItem->iSelectId);
    }else{
      zMsg = sqlite3MAppendf(db, zMsg, "%s TABLE %s", zMsg, pItem->zName);
    }

    if( pItem->zAlias ){
      zMsg = sqlite3MAppendf(db, zMsg, "%s AS %s", zMsg, pItem->zAlias);
    }
    if( (flags & WHERE_INDEXED)!=0 ){
      char *zWhere = explainIndexRange(db, pLevel, pItem->pTab);
      zMsg = sqlite3MAppendf(db, zMsg, "%s USING %s%sINDEX%s%s%s", zMsg, 
          ((flags & WHERE_TEMP_INDEX)?"AUTOMATIC ":""),
          ((flags & WHERE_IDX_ONLY)?"COVERING ":""),
          ((flags & WHERE_TEMP_INDEX)?"":" "),
          ((flags & WHERE_TEMP_INDEX)?"": pLevel->plan.u.pIdx->zName),
          zWhere
      );
      sqlite3DbFree(db, zWhere);
    }else if( flags & (WHERE_ROWID_EQ|WHERE_ROWID_RANGE) ){
      zMsg = sqlite3MAppendf(db, zMsg, "%s USING INTEGER PRIMARY KEY", zMsg);

      if( flags&WHERE_ROWID_EQ ){
        zMsg = sqlite3MAppendf(db, zMsg, "%s (rowid=?)", zMsg);
      }else if( (flags&WHERE_BOTH_LIMIT)==WHERE_BOTH_LIMIT ){
        zMsg = sqlite3MAppendf(db, zMsg, "%s (rowid>? AND rowid<?)", zMsg);
      }else if( flags&WHERE_BTM_LIMIT ){
        zMsg = sqlite3MAppendf(db, zMsg, "%s (rowid>?)", zMsg);
      }else if( flags&WHERE_TOP_LIMIT ){
        zMsg = sqlite3MAppendf(db, zMsg, "%s (rowid<?)", zMsg);
      }
    }
#ifndef SQLITE_OMIT_VIRTUALTABLE
    else if( (flags & WHERE_VIRTUALTABLE)!=0 ){
      sqlite3_index_info *pVtabIdx = pLevel->plan.u.pVtabIdx;
      zMsg = sqlite3MAppendf(db, zMsg, "%s VIRTUAL TABLE INDEX %d:%s", zMsg,
                  pVtabIdx->idxNum, pVtabIdx->idxStr);
    }
#endif
    if( wctrlFlags&(WHERE_ORDERBY_MIN|WHERE_ORDERBY_MAX) ){
      testcase( wctrlFlags & WHERE_ORDERBY_MIN );
      nRow = 1;
    }else{
      nRow = (sqlite3_int64)pLevel->plan.nRow;
    }
    zMsg = sqlite3MAppendf(db, zMsg, "%s (~%lld rows)", zMsg, nRow);
    sqlite3VdbeAddOp4(v, OP_Explain, iId, iLevel, iFrom, zMsg, P4_DYNAMIC);
  }
}
#else
# define explainOneScan(u,v,w,x,y,z)
#endif /* SQLITE_OMIT_EXPLAIN */


/*
** Generate code for the start of the iLevel-th loop in the WHERE clause
** implementation described by pWInfo.
<<<<<<< HEAD
���ɴ��룬iLevel-thѭ����ʼ��pWInfo��WHERE�Ӿ���ʵ��
*/
static Bitmask codeOneLoopStart(
  WhereInfo *pWInfo,   /*������WHERE�Ӿ����Ϣ */
  int iLevel,          /*����pWInfo->a[]*/
  u16 wctrlFlags,      /*sqliteInt.h�ж����WHERE_ *��־֮һ*/
  Bitmask notReady     /* ��ǰ�����Ч�ռ� */
){
  int j, k;            /* ѭ�������� */
  int iCur;            /* ����α� */
  int addrNxt;         /* ��ʲôʱ������ѭ��������һ��in */
  int omitTable;       /* ֵΪ�棬�������ֻʹ������ */
  int bRev;            /* ֵΪ�棬������ǵ���ɨ�� */
  WhereLevel *pLevel;  /* ˮƽ����*/
  WhereClause *pWC;    /*����where�Ӿ�ķֽ� */
  WhereTerm *pTerm;               /* where�Ӿ� */
  Parse *pParse;                  /* ���������� */
  Vdbe *v;                        /* ׼��֧�Žṹ */
  struct SrcList_item *pTabItem;  /* FROM�Ӿ���� */
  int addrBrk;                    /* ����ѭ��*/
  int addrCont;                   /* ����ѭ��������һ���� */
  int iRowidReg = 0;        /*Rowid�洢������Ĵ���,���������*/
  int iReleaseReg = 0;      /* �ڷ���ǰ���ͷ���ʱ�Ĵ��� */
=======
**
** ͨ��pWInfo��������ΪWHERE�Ӿ���ʵ�ֵĵĵ�i��ѭ���Ŀ�ʼ���ɴ���
*/
static Bitmask codeOneLoopStart(
  WhereInfo *pWInfo,   /* Complete information about the WHERE clause WHERE�Ӿ��������Ϣ */
  int iLevel,          /* Which level of pWInfo->a[] should be coded ��Ҫ�����pWInfo->a[]�ĵȼ� */
  u16 wctrlFlags,      /* One of the WHERE_* flags defined in sqliteInt.h ��sqliteInt.h�ж����WHERE_*��־�е�һ�� */
  Bitmask notReady     /* Which tables are currently available �ĸ���ʾ��ǰ��Ч�� */
){
  int j, k;            /* Loop counters ѭ�������� */
  int iCur;            /* The VDBE cursor for the table ���VDBE�α� */
  int addrNxt;         /* Where to jump to continue with the next IN case ��ת������һ��IN */
  int omitTable;       /* True if we use the index only �������ֻʹ��������ΪTRUE */
  int bRev;            /* True if we need to scan in reverse order ���������Ҫ�ڵ�����ɨ����ΪTRUE */
  WhereLevel *pLevel;  /* The where level to be coded �������where�ȼ� */
  WhereClause *pWC;    /* Decomposition of the entire WHERE clause ����WHERE�Ӿ�ķֽ� */
  WhereTerm *pTerm;               /* A WHERE clause term һ��WHERE�Ӿ��term */
  Parse *pParse;                  /* Parsing context ���������� */
  Vdbe *v;                        /* The prepared stmt under constructions �ڹ�����׼���õ�stmt */
  struct SrcList_item *pTabItem;  /* FROM clause term being coded ���ڱ����FROM�Ӿ�term */
  int addrBrk;                    /* Jump here to break out of the loop ����ѭ��ʱ��λ�� */
  int addrCont;                   /* Jump here to continue with next cycle ������һ��ѭ����λ�� */
  int iRowidReg = 0;        /* Rowid is stored in this register, if not zero �����Ϊ0��Rowid�洢������Ĵ����� */
  int iReleaseReg = 0;      /* Temp register to free before returning �ڷ���ǰ�ͷ���ʱ�Ĵ��� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

  pParse = pWInfo->pParse;
  v = pParse->pVdbe;
  pWC = pWInfo->pWC;
  pLevel = &pWInfo->a[iLevel];
  pTabItem = &pWInfo->pTabList->a[pLevel->iFrom];
  iCur = pTabItem->iCursor;
  bRev = (pLevel->plan.wsFlags & WHERE_REVERSE)!=0;
  omitTable = (pLevel->plan.wsFlags & WHERE_IDX_ONLY)!=0 
           && (wctrlFlags & WHERE_FORCE_TABLE)==0;

  /* Create labels for the "break" and "continue" instructions
  ** for the current loop.  Jump to addrBrk to break out of a loop.
  ** Jump to cont to go immediately to the next iteration of the
  ** loop.
<<<<<<< HEAD
  **����ǩ�ġ�break���͡�continue��Ϊ��ǰѭ����
  ��ת��addrBrkΪ�˴���ѭ����
  ����������һ������ѭ��
=======
  **
  ** Ϊ��ǰѭ����"break"��"continue"ָ��Ĵ�����ǩ������addrBrk������ѭ����
  ** ����addrCont������ִ����һ��ѭ��
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  ** When there is an IN operator, we also have a "addrNxt" label that
  ** means to continue with the next IN value combination.  When
  ** there are no IN operators in the constraints, the "addrNxt" label
  ** is the same as "addrBrk".
<<<<<<< HEAD
  ����һ��IN operator,����Ҳ��һ����addrNxt���ı�ǩ,
  ����ζ��,����������һ��IN value��ϡ�
  ��û��IN operators������,��addrNxt����ǩ�͡�addrBrk��һ��
=======
  ** ����һ��IN�������"addrNxt"��ǩ��ʾ������һ��INֵ��ϡ�
  ** ��Լ����û��IN�����ʱ��"addrNxt"��ǩ��"addrBrk"������ͬ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  addrBrk = pLevel->addrBrk = pLevel->addrNxt = sqlite3VdbeMakeLabel(v);
  addrCont = pLevel->addrCont = sqlite3VdbeMakeLabel(v);

  /* If this is the right table of a LEFT OUTER JOIN, allocate and
  ** initialize a memory cell that records if this table matches any
  ** row of the left table of the join.
<<<<<<< HEAD
  ���������ȷ���������ӱ�,����ͳ�ʼ��һ���洢��Ԫ,
  ��¼�˱�ƥ���κ����������ӡ�
=======
  **
  ** �������һ��LEFT OUTER JOIN���ұ����䲢��ʼ��һ���ڴ浥Ԫ����¼�˱�ƥ���join�е�������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pLevel->iFrom>0 && (pTabItem[0].jointype & JT_LEFT)!=0 ){
    pLevel->iLeftJoin = ++pParse->nMem;
    sqlite3VdbeAddOp2(v, OP_Integer, 0, pLevel->iLeftJoin);
    VdbeComment((v, "init LEFT JOIN no-match flag"));
  }

#ifndef SQLITE_OMIT_VIRTUALTABLE
  if(  (pLevel->plan.wsFlags & WHERE_VIRTUALTABLE)!=0 ){
    /* Case 0:  The table is a virtual-table.  Use the VFilter and VNext
    **          to access the data.
<<<<<<< HEAD
    ����һ�������ʹ��VFilter��VNext��������
=======
    **
    ** ���0:����һ�������ʹ��VFilter��VNext���������ݡ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    int iReg;   /* P3 Value for OP_VFilter OP_VFilter��P3ֵ */
    sqlite3_index_info *pVtabIdx = pLevel->plan.u.pVtabIdx;
    int nConstraint = pVtabIdx->nConstraint;
    struct sqlite3_index_constraint_usage *aUsage =
                                                pVtabIdx->aConstraintUsage;
    const struct sqlite3_index_constraint *aConstraint =
                                                pVtabIdx->aConstraint;

    sqlite3ExprCachePush(pParse);
    iReg = sqlite3GetTempRange(pParse, nConstraint+2);
    for(j=1; j<=nConstraint; j++){
      for(k=0; k<nConstraint; k++){
        if( aUsage[k].argvIndex==j ){
          int iTerm = aConstraint[k].iTermOffset;
          sqlite3ExprCode(pParse, pWC->a[iTerm].pExpr->pRight, iReg+j+1);
          break;
        }
      }
      if( k==nConstraint ) break;
    }
    sqlite3VdbeAddOp2(v, OP_Integer, pVtabIdx->idxNum, iReg);
    sqlite3VdbeAddOp2(v, OP_Integer, j-1, iReg+1);
    sqlite3VdbeAddOp4(v, OP_VFilter, iCur, addrBrk, iReg, pVtabIdx->idxStr,
                      pVtabIdx->needToFreeIdxStr ? P4_MPRINTF : P4_STATIC);
    pVtabIdx->needToFreeIdxStr = 0;
    for(j=0; j<nConstraint; j++){
      if( aUsage[j].omit ){
        int iTerm = aConstraint[j].iTermOffset;
        disableTerm(pLevel, &pWC->a[iTerm]);
      }
    }
    pLevel->op = OP_VNext;
    pLevel->p1 = iCur;
    pLevel->p2 = sqlite3VdbeCurrentAddr(v);
    sqlite3ReleaseTempRange(pParse, iReg, nConstraint+2);
    sqlite3ExprCachePop(pParse, 1);
  }else
#endif /* SQLITE_OMIT_VIRTUALTABLE */

  if( pLevel->plan.wsFlags & WHERE_ROWID_EQ ){
    /* Case 1:  We can directly reference a single row using an
    **          equality comparison against the ROWID field.  Or
    **          we reference multiple rows using a "rowid IN (...)"
    **          construct.
<<<<<<< HEAD
    ���ǿ���ֱ������һ�����ж�ROWID�ֶ�ʹ����ȵıȽϡ�
    ��������ʹ�����ö���С�rowid(��)IN�����졣
=======
    **
    ** ���1:���ǿ���ֱ������һ������ʹ�õ�ʽ��ROWID�ֶαȽϡ�
    **       �����ö���ʹ��"rowid IN (...)"�ṹ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    iReleaseReg = sqlite3GetTempReg(pParse);
    pTerm = findTerm(pWC, iCur, -1, notReady, WO_EQ|WO_IN, 0);
    assert( pTerm!=0 );
    assert( pTerm->pExpr!=0 );
    assert( pTerm->leftCursor==iCur );
    assert( omitTable==0 );
    testcase( pTerm->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
    iRowidReg = codeEqualityTerm(pParse, pTerm, pLevel, iReleaseReg);
    addrNxt = pLevel->addrNxt;
    sqlite3VdbeAddOp2(v, OP_MustBeInt, iRowidReg, addrNxt);
    sqlite3VdbeAddOp3(v, OP_NotExists, iCur, addrNxt, iRowidReg);
    sqlite3ExprCacheStore(pParse, iCur, -1, iRowidReg);
    VdbeComment((v, "pk"));
    pLevel->op = OP_Noop;
  }else if( pLevel->plan.wsFlags & WHERE_ROWID_RANGE ){
    /* Case 2:  We have an inequality comparison against the ROWID field.
<<<<<<< HEAD
    ���Ƕ�ROWID�ֶ��в���ȵıȽ�
=======
    **
    ** ���2:��һ����ROWID�ֶν��еĲ���ʽ�Ƚ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    int testOp = OP_Noop;
    int start;
    int memEndValue = 0;
    WhereTerm *pStart, *pEnd;

    assert( omitTable==0 );
    pStart = findTerm(pWC, iCur, -1, notReady, WO_GT|WO_GE, 0);
    pEnd = findTerm(pWC, iCur, -1, notReady, WO_LT|WO_LE, 0);
    if( bRev ){
      pTerm = pStart;
      pStart = pEnd;
      pEnd = pTerm;
    }
    if( pStart ){
<<<<<<< HEAD
      Expr *pX;             /* ���ʽ,�����˿�ʼ*/
      int r1, rTemp;        /* �Ĵ�����ʼ���ɱ߽� */

      /* The following constant maps TK_xx codes into corresponding 
      ** seek opcodes.  It depends on a particular ordering of TK_xx
      ���г���TK_xx����ӳ�䵽��Ӧ�Ĳ����롣
      ��ȡ����һ���ض�TK_xxָʾ
=======
      Expr *pX;             /* The expression that defines the start bound ���ʽ�����˿�ʼ��Χ */
      int r1, rTemp;        /* Registers for holding the start boundary ���濪ʼ��Χ�ļĴ��� */

      /* The following constant maps TK_xx codes into corresponding 
      ** seek opcodes.  It depends on a particular ordering of TK_xx
      **
      ** ����ĳ�������TK_xx�����˳�򣬰�TK_xxӳ�䵽��Ӧ�����������롣
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      */
      const u8 aMoveOp[] = {
           /* TK_GT */  OP_SeekGt,
           /* TK_LE */  OP_SeekLe,
           /* TK_LT */  OP_SeekLt,
           /* TK_GE */  OP_SeekGe
      };
      assert( TK_LE==TK_GT+1 );      /* Make sure the ordering.. ȷ��˳�� */
      assert( TK_LT==TK_GT+2 );      /*  ... of the TK_xx values...  */
      assert( TK_GE==TK_GT+3 );      /*  ... is correcct. */

      testcase( pStart->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
      pX = pStart->pExpr;
      assert( pX!=0 );
      assert( pStart->leftCursor==iCur );
      r1 = sqlite3ExprCodeTemp(pParse, pX->pRight, &rTemp);
      sqlite3VdbeAddOp3(v, aMoveOp[pX->op-TK_GT], iCur, addrBrk, r1);
      VdbeComment((v, "pk"));
      sqlite3ExprCacheAffinityChange(pParse, r1, 1);
      sqlite3ReleaseTempReg(pParse, rTemp);
      disableTerm(pLevel, pStart);
    }else{
      sqlite3VdbeAddOp2(v, bRev ? OP_Last : OP_Rewind, iCur, addrBrk);
    }
    if( pEnd ){
      Expr *pX;
      pX = pEnd->pExpr;
      assert( pX!=0 );
      assert( pEnd->leftCursor==iCur );
      testcase( pEnd->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
      memEndValue = ++pParse->nMem;
      sqlite3ExprCode(pParse, pX->pRight, memEndValue);
      if( pX->op==TK_LT || pX->op==TK_GT ){
        testOp = bRev ? OP_Le : OP_Ge;
      }else{
        testOp = bRev ? OP_Lt : OP_Gt;
      }
      disableTerm(pLevel, pEnd);
    }
    start = sqlite3VdbeCurrentAddr(v);
    pLevel->op = bRev ? OP_Prev : OP_Next;
    pLevel->p1 = iCur;
    pLevel->p2 = start;
    if( pStart==0 && pEnd==0 ){
      pLevel->p5 = SQLITE_STMTSTATUS_FULLSCAN_STEP;
    }else{
      assert( pLevel->p5==0 );
    }
    if( testOp!=OP_Noop ){
      iRowidReg = iReleaseReg = sqlite3GetTempReg(pParse);
      sqlite3VdbeAddOp2(v, OP_Rowid, iCur, iRowidReg);
      sqlite3ExprCacheStore(pParse, iCur, -1, iRowidReg);
      sqlite3VdbeAddOp3(v, testOp, memEndValue, addrBrk, iRowidReg);
      sqlite3VdbeChangeP5(v, SQLITE_AFF_NUMERIC | SQLITE_JUMPIFNULL);
    }
  }else if( pLevel->plan.wsFlags & (WHERE_COLUMN_RANGE|WHERE_COLUMN_EQ) ){
    /* Case 3: A scan using an index.
    **
    **         The WHERE clause may contain zero or more equality 
    **         terms ("==" or "IN" operators) that refer to the N
    **         left-most columns of the index. It may also contain
    **         inequality constraints (>, <, >= or <=) on the indexed
    **         column that immediately follows the N equalities. Only 
    **         the right-most column can be an inequality - the rest must
    **         use the "==" and "IN" operators. For example, if the 
    **         index is on (x,y,z), then the following clauses are all 
    **         optimized:
    **
    **            x=5
    **            x=5 AND y=10
    **            x=5 AND y<10
    **            x=5 AND y>5 AND y<10
    **            x=5 AND y=5 AND z<=10
    **
    **         The z<10 term of the following cannot be used, only
    **         the x=5 term:
    **
    **            x=5 AND z<10
    **
    **         N may be zero if there are inequality constraints.
    **         If there are no inequality constraints, then N is at
    **         least one.
    **
    **         This case is also used when there are no WHERE clause
    **         constraints but an index is selected anyway, in order
    **         to force the output order to conform to an ORDER BY.
<<<<<<< HEAD
    where�Ӿ������0�����߶����ȹ�ϵ��"==" or "IN" ���漰��N��������������
    ������Ҳ�в��ȹ�ϵ(>, <, >= or <=)���������н����ţ���ȹ�ϵ��
    ֻ���������Ӽ��ſ��Գ�Ϊ��ȣ�ʣ��ı������� "==" and "IN" ��
    ������(x,y,z)�����������������е��Ӿ䶼�����ŵġ�
    ��������������x=5
=======
    **
    ** ���3:ʹ��������ɨ��
    **         WHERE�Ӿ���ܰ���0������ʽ
    **         terms ("=="��"IN"�����)�漰��N����������ߵ���.
    **         ����������������Ҳ��������ʽԼ��(>, <, >= or <=)�����������N����ʽ��
    **         ֻ�����ұߵ��п���Ϊһ������ʽ--����ı���ʹ��"=="��"IN"�������
    **         ���磬�����һ����(x,y,z)��������������Ӿ䶼���Խ����Ż�:
    **            x=5
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    **            x=5 AND y=10
    **            x=5 AND y<10
    **            x=5 AND y>5 AND y<10
    **            x=5 AND y=5 AND z<=10
<<<<<<< HEAD
    �����z<10�����õġ���Ϊ����x=5Լ��
    N������0�������в���Լ����
    �������û�в���������ôN����Ϊ1.
    
    ���������Ҳû��WHERE�Ӿ�Լ��������ѡ��ʹ��,
    Ϊ����ʹ���˳�����ORDER BY��
=======
    **         �����������z<10�����Ż���ֻ��x=5����:
    **            x=5 AND z<10
    **         ����в���ʽԼ����ôN����Ϊ0.���û�в���ʽԼ����N����Ϊ1.
    **         
    **         ��û��WHERE�Ӿ�Լ������ѡ����һ����������Ϊ��ǿ�����������ORDER BY˳��ʱ���������Ҳ�����õġ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */  
    static const u8 aStartOp[] = {
      0,
      0,
      OP_Rewind,           /* 2: (!start_constraints && startEq &&  !bRev) */
      OP_Last,             /* 3: (!start_constraints && startEq &&   bRev) */
      OP_SeekGt,           /* 4: (start_constraints  && !startEq && !bRev) */
      OP_SeekLt,           /* 5: (start_constraints  && !startEq &&  bRev) */
      OP_SeekGe,           /* 6: (start_constraints  &&  startEq && !bRev) */
      OP_SeekLe            /* 7: (start_constraints  &&  startEq &&  bRev) */
    };
    static const u8 aEndOp[] = {
      OP_Noop,             /* 0: (!end_constraints) */
      OP_IdxGE,            /* 1: (end_constraints && !bRev) */
      OP_IdxLT             /* 2: (end_constraints && bRev) */
    };
<<<<<<< HEAD
    int nEq = pLevel->plan.nEq;  /* Number of == or IN terms */
    int isMinQuery = 0;          /* �������һ�� SELECT min(x)..���Ż� */
    int regBase;                 /* ��ַ�Ĵ���Լ��ֵ */
    int r1;                      /* ��ʱ�Ĵ���  */
    WhereTerm *pRangeStart = 0;  /* ����Լ����ʼ*/
    WhereTerm *pRangeEnd = 0;    /* ����Լ���ķ�Χ */
    int startEq;                 /* True if range start uses ==, >= or <= */
    int endEq;                   /* True if range end uses ==, >= or <= */
    int start_constraints;       /* ���޷�Χ�Ŀ�ʼ */
    int nConstraint;             /* Լ����������*/
    Index *pIdx;                 /* ���ǽ�ʹ������ */
    int iIdxCur;                 /* VDBE�α������*/
    int nExtraReg = 0;           /* ����Ķ���ļĴ������� */
    int op;                      /* ָ�������  */
    char *zStartAff;             /*AffinityԼ����Χ�Ŀ�ʼ */
    char *zEndAff;               /* AffinityԼ����Χ�Ľ��� */
=======
    int nEq = pLevel->plan.nEq;  /* Number of == or IN terms ==��INterms����Ŀ */
    int isMinQuery = 0;          /* If this is an optimized SELECT min(x).. �������һ���Ż���SELECT min(x)��� */
    int regBase;                 /* Base register holding constraint values ��ַ�Ĵ�������Լ��ֵ */
    int r1;                      /* Temp register ��ʱ�Ĵ��� */
    WhereTerm *pRangeStart = 0;  /* Inequality constraint at range start �ڷ�Χ��ʼ�Ĳ���ʽԼ�� */
    WhereTerm *pRangeEnd = 0;    /* Inequality constraint at range end �ڷ�Χĩβ�Ĳ���ʽԼ�� */
    int startEq;                 /* True if range start uses ==, >= or <= �����Χ��ʼʹ��==, >= or <=,��ôΪTRUE */
    int endEq;                   /* True if range end uses ==, >= or <= �����Χĩβʹ��==, >= or <=,��ôΪTRUE */
    int start_constraints;       /* Start of range is constrained ��Χ��ʼ����Լ���� */
    int nConstraint;             /* Number of constraint terms Լ��terms����Ŀ */
    Index *pIdx;                 /* The index we will be using ��ʹ�õ����� */
    int iIdxCur;                 /* The VDBE cursor for the index ������VDBE�α� */
    int nExtraReg = 0;           /* Number of extra registers needed ��Ҫ�Ķ���ļĴ������� */
    int op;                      /* Instruction opcode ָʾ������ */
    char *zStartAff;             /* Affinity for start of range constraint ��Χ��ʼ��Լ�����׺��� */
    char *zEndAff;               /* Affinity for end of range constraint ��Χĩβ��Լ�����׺��� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

    pIdx = pLevel->plan.u.pIdx;
    iIdxCur = pLevel->iIdxCur;
    k = (nEq==pIdx->nColumn ? -1 : pIdx->aiColumn[nEq]);

    /* If this loop satisfies a sort order (pOrderBy) request that 
    ** was passed to this function to implement a "SELECT min(x) ..." 
    ** query, then the caller will only allow the loop to run for
    ** a single iteration. This means that the first row returned
    ** should not have a NULL value stored in 'x'. If column 'x' is
    ** the first one after the nEq equality constraints in the index,
    ** this requires some special handling.
<<<<<<< HEAD
    ������ѭ����������˳��(pOrderBy)
    ���󴫵ݸ��������ʵ��һ����ѡ��min(x)������ѯ,
    ��ô�����߽�ֻ����Ϊһ������ѭ�����С�
    ����ζ�ŵ�һ�в�Ӧ�÷���NULLֵ�洢�ڡ�x����
    ����С�x����ĵ�һ��nEq��ʽԼ����ָ��,����ҪһЩ����Ĵ���
=======
    **
    ** ������ѭ������һ������˳��(pOrderBy)�����󣬴��ݸ��������ʵ��һ��"SELECT min(x) ..."��ѯ��
    ** ��ô������ֻ����ѭ��Ϊһ���������С�����ζ�ŷ��صĵ�һ�в�����NULLֵ�洢��'x'�С�
    ** �����'x'��������nEq����ʽԼ�����һ�����������һЩ�ر���
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( (wctrlFlags&WHERE_ORDERBY_MIN)!=0
     && (pLevel->plan.wsFlags&WHERE_ORDERBY)
     && (pIdx->nColumn>nEq)
    ){
      /* assert( pOrderBy->nExpr==1 ); */
      /* assert( pOrderBy->a[0].pExpr->iColumn==pIdx->aiColumn[nEq] ); */
      isMinQuery = 1;
      nExtraReg = 1;
    }

    /* Find any inequality constraint terms for the start and end 
    ** of the range. 
<<<<<<< HEAD
    �ҵ��κβ���ʽԼ��������Χ�Ŀ�ʼ�ͽ�����
=======
    **
    ** Ϊ��Χ�Ŀ�ʼ�ͽ�β���Ҳ���ʽԼ��terms
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( pLevel->plan.wsFlags & WHERE_TOP_LIMIT ){
      pRangeEnd = findTerm(pWC, iCur, k, notReady, (WO_LT|WO_LE), pIdx);
      nExtraReg = 1;
    }
    if( pLevel->plan.wsFlags & WHERE_BTM_LIMIT ){
      pRangeStart = findTerm(pWC, iCur, k, notReady, (WO_GT|WO_GE), pIdx);
      nExtraReg = 1;
    }

    /* Generate code to evaluate all constraint terms using == or IN
    ** and store the values of those terms in an array of registers
    ** starting at regBase.
<<<<<<< HEAD
    ʹ��= = enerate��������������Լ������������Щ����
    ��ֵ�洢��һ��������regBase�Ĵ����Ŀ�ʼ��
=======
    **
    ** ���ɴ�������������ʹ��==��INԼ��terms���Ұ���Щterms��ֵ�洢����regBase��ʼ��һ��Ĵ����С�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    regBase = codeAllEqualityTerms(
        pParse, pLevel, pWC, notReady, nExtraReg, &zStartAff
    );
    zEndAff = sqlite3DbStrDup(pParse->db, zStartAff);
    addrNxt = pLevel->addrNxt;

    /* If we are doing a reverse order scan on an ascending index, or
    ** a forward order scan on a descending index, interchange the 
    ** start and end terms (pRangeStart and pRangeEnd).
<<<<<<< HEAD
    ���������һ������ɨ��һ����������,
    ��ת��˳��������ɨ��,
    �����Ŀ�ʼ�ͽ�������(pRangeStart��pRangeEnd)
=======
    **
    ** ���������������������һ������ɨ�裬������һ��������������һ������ɨ�裬������ʼ�ͽ�����terms(pRangeStart and pRangeEnd).
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( (nEq<pIdx->nColumn && bRev==(pIdx->aSortOrder[nEq]==SQLITE_SO_ASC))
     || (bRev && pIdx->nColumn==nEq)
    ){
      SWAP(WhereTerm *, pRangeEnd, pRangeStart);
    }

    testcase( pRangeStart && pRangeStart->eOperator & WO_LE );
    testcase( pRangeStart && pRangeStart->eOperator & WO_GE );
    testcase( pRangeEnd && pRangeEnd->eOperator & WO_LE );
    testcase( pRangeEnd && pRangeEnd->eOperator & WO_GE );
    startEq = !pRangeStart || pRangeStart->eOperator & (WO_LE|WO_GE);
    endEq =   !pRangeEnd || pRangeEnd->eOperator & (WO_LE|WO_GE);
    start_constraints = pRangeStart || nEq>0;

<<<<<<< HEAD
    /* Seek the index cursor to the start of the range.
    Ѱ������ָ�뷶Χ�Ŀ�ʼ��
     */
=======
    /* Seek the index cursor to the start of the range. ��ѯ�����α귶Χ�Ŀ�ʼ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    nConstraint = nEq;
    if( pRangeStart ){
      Expr *pRight = pRangeStart->pExpr->pRight;
      sqlite3ExprCode(pParse, pRight, regBase+nEq);
      if( (pRangeStart->wtFlags & TERM_VNULL)==0 ){
        sqlite3ExprCodeIsNullJump(v, pRight, regBase+nEq, addrNxt);
      }
      if( zStartAff ){
        if( sqlite3CompareAffinity(pRight, zStartAff[nEq])==SQLITE_AFF_NONE){
          /* Since the comparison is to be performed with no conversions
          ** applied to the operands, set the affinity to apply to pRight to 
<<<<<<< HEAD
          ** SQLITE_AFF_NONE.
          ���ڱȽ���û��ִ��ת��Ӧ���ڲ�����,
          ���ù���Ӧ��pRight SQLITE_AFF_NONE��
            */
=======
          ** SQLITE_AFF_NONE.  
          **
          ** ����Ӧ������������ϵıȽ���δת���ģ�����pRight���׺���ΪSQLITE_AFF_NONE
          */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          zStartAff[nEq] = SQLITE_AFF_NONE;
        }
        if( sqlite3ExprNeedsNoAffinityChange(pRight, zStartAff[nEq]) ){
          zStartAff[nEq] = SQLITE_AFF_NONE;
        }
      }  
      nConstraint++;
      testcase( pRangeStart->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
    }else if( isMinQuery ){
      sqlite3VdbeAddOp2(v, OP_Null, 0, regBase+nEq);
      nConstraint++;
      startEq = 0;
      start_constraints = 1;
    }
    codeApplyAffinity(pParse, regBase, nConstraint, zStartAff);
    op = aStartOp[(start_constraints<<2) + (startEq<<1) + bRev];
    assert( op!=0 );
    testcase( op==OP_Rewind );
    testcase( op==OP_Last );
    testcase( op==OP_SeekGt );
    testcase( op==OP_SeekGe );
    testcase( op==OP_SeekLe );
    testcase( op==OP_SeekLt );
    sqlite3VdbeAddOp4Int(v, op, iIdxCur, addrNxt, regBase, nConstraint);

    /* Load the value for the inequality constraint at the end of the
    ** range (if any).
<<<<<<< HEAD
    ���ز���ʽԼ���ķ�Χ��ֵ(����еĻ�)
=======
    **
    ** Ϊ��Χ��ĩβ�Ĳ���ʽԼ������ֵ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    nConstraint = nEq;
    if( pRangeEnd ){
      Expr *pRight = pRangeEnd->pExpr->pRight;
      sqlite3ExprCacheRemove(pParse, regBase+nEq, 1);
      sqlite3ExprCode(pParse, pRight, regBase+nEq);
      if( (pRangeEnd->wtFlags & TERM_VNULL)==0 ){
        sqlite3ExprCodeIsNullJump(v, pRight, regBase+nEq, addrNxt);
      }
      if( zEndAff ){
        if( sqlite3CompareAffinity(pRight, zEndAff[nEq])==SQLITE_AFF_NONE){
          /* Since the comparison is to be performed with no conversions
          ** applied to the operands, set the affinity to apply to pRight to 
<<<<<<< HEAD
          ** SQLITE_AFF_NONE. 
          ���ڱȽ���û��ִ��ת��Ӧ���ڲ�����,
          ���ù�������pRight SQLITE_AFF_NONE��
           */
=======
          ** SQLITE_AFF_NONE.  
          **
          ** ����Ӧ������������ϵıȽ���δת���ģ�����pRight���׺���ΪSQLITE_AFF_NONE
          */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          zEndAff[nEq] = SQLITE_AFF_NONE;
        }
        if( sqlite3ExprNeedsNoAffinityChange(pRight, zEndAff[nEq]) ){
          zEndAff[nEq] = SQLITE_AFF_NONE;
        }
      }  
      codeApplyAffinity(pParse, regBase, nEq+1, zEndAff);
      nConstraint++;
      testcase( pRangeEnd->wtFlags & TERM_VIRTUAL ); /* EV: R-30575-11662 */
    }
    sqlite3DbFree(pParse->db, zStartAff);
    sqlite3DbFree(pParse->db, zEndAff);

    /* Top of the loop body ѭ����Ķ��� */
    pLevel->p2 = sqlite3VdbeCurrentAddr(v);

<<<<<<< HEAD
    /* Check if the index cursor is past the end of the range. 
    �������ָ���Ƿ������ȥ�ķ�Χ��
    */
=======
    /* Check if the index cursor is past the end of the range. ��������α��Ƿ񾭹���Χ��ĩβ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    op = aEndOp[(pRangeEnd || nEq) * (1 + bRev)];
    testcase( op==OP_Noop );
    testcase( op==OP_IdxGE );
    testcase( op==OP_IdxLT );
    if( op!=OP_Noop ){
      sqlite3VdbeAddOp4Int(v, op, iIdxCur, addrNxt, regBase, nConstraint);
      sqlite3VdbeChangeP5(v, endEq!=bRev ?1:0);
    }

    /* If there are inequality constraints, check that the value
    ** of the table column that the inequality contrains is not NULL.
    ** If it is, jump to the next iteration of the loop.
<<<<<<< HEAD
    ����в���ʽԼ��,�����е�ֵ,
    ��ƽ��contrains���ǿյġ�
    �����,��ת����һ��������ѭ����
=======
    **
    ** ����в���ʽԼ�����������в���ʽԼ�����е�ֵ�Ƿ�ΪNULL.
    ** �����ΪNULL��������һ��ѭ������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    r1 = sqlite3GetTempReg(pParse);
    testcase( pLevel->plan.wsFlags & WHERE_BTM_LIMIT );
    testcase( pLevel->plan.wsFlags & WHERE_TOP_LIMIT );
    if( (pLevel->plan.wsFlags & (WHERE_BTM_LIMIT|WHERE_TOP_LIMIT))!=0 ){
      sqlite3VdbeAddOp3(v, OP_Column, iIdxCur, nEq, r1);
      sqlite3VdbeAddOp2(v, OP_IsNull, r1, addrCont);
    }
    sqlite3ReleaseTempReg(pParse, r1);

<<<<<<< HEAD
    /* Seek the table cursor, if required 
    Ѱ���ָ��,�����Ҫ�Ļ�
    */
=======
    /* Seek the table cursor, if required �����Ҫ����ѯ���α� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    disableTerm(pLevel, pRangeStart);
    disableTerm(pLevel, pRangeEnd);
    if( !omitTable ){
      iRowidReg = iReleaseReg = sqlite3GetTempReg(pParse);
      sqlite3VdbeAddOp2(v, OP_IdxRowid, iIdxCur, iRowidReg);
      sqlite3ExprCacheStore(pParse, iCur, -1, iRowidReg);
      sqlite3VdbeAddOp2(v, OP_Seek, iCur, iRowidReg);  /* Deferred seek */
    }

    /* Record the instruction used to terminate the loop. Disable 
    ** WHERE clause terms made redundant by the index range scan.
<<<<<<< HEAD
    ��¼ʹ�õ�ָ����ֹѭ��������WHERE�Ӿ���������Χɨ����ɵ����ࡣ
=======
    **
    ** ��¼���ڽ���ѭ����ָ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( pLevel->plan.wsFlags & WHERE_UNIQUE ){
      pLevel->op = OP_Noop;
    }else if( bRev ){
      pLevel->op = OP_Prev;
    }else{
      pLevel->op = OP_Next;
    }
    pLevel->p1 = iIdxCur;
  }else

#ifndef SQLITE_OMIT_OR_OPTIMIZATION
  if( pLevel->plan.wsFlags & WHERE_MULTI_OR ){
    /* Case 4:  Two or more separately indexed terms connected by OR
    **�������������ϵĶ����������������
    ** Example:
    **
    **   CREATE TABLE t1(a,b,c,d);
    **   CREATE INDEX i1 ON t1(a);
    **   CREATE INDEX i2 ON t1(b);
    **   CREATE INDEX i3 ON t1(c);
    **
    **   SELECT * FROM t1 WHERE a=5 OR b=7 OR (c=11 AND d=13)
    **
    ** In the example, there are three indexed terms connected by OR.
    ** The top of the loop looks like this:
    **�����������,������������������ӡ�
    **          Null       1                # Zero the rowset in reg 1
    **
    ** Then, for each indexed term, the following. The arguments to
    ** RowSetTest are such that the rowid of the current row is inserted
    ** into the RowSet. If it is already present, control skips the
    ** Gosub opcode and jumps straight to the code generated by WhereEnd().
    **Ȼ��,Ϊÿ����������,���档
    RowSet�Ĳ���,rowid��ǰ�в��뵽�м���
    ������Ѿ�����,��WhereEnd()��������Gosub���������Ծֱ�����ɵĴ��롣
    **        sqlite3WhereBegin(<term>)
    **          RowSetTest                  # Insert rowid into rowset
    **          Gosub      2 A
    **        sqlite3WhereEnd()
    **
    ** Following the above, code to terminate the loop. Label A, the target
    ** of the Gosub above, jumps to the instruction right after the Goto.
    **������Ŀ�֪������Ĵ�����ֹѭ������ǩA����Gosub��Ŀ��,��תָ��֮��ת����
    **          Null       1                # Zero the rowset in reg 1
    **          Goto       B                # The loop is finished.
    **
    **       A: <loop body>                 # Return data, whatever.
    **
    **          Return     2                # Jump back to the Gosub
    **
    **       B: <after the loop>
    **
    ** ���4:  ��OR���ӵ����������Ķ�������terms.
    ** ����:
    **
    **   CREATE TABLE t1(a,b,c,d);
    **   CREATE INDEX i1 ON t1(a);
    **   CREATE INDEX i2 ON t1(b);
    **   CREATE INDEX i3 ON t1(c);
    **
    **   SELECT * FROM t1 WHERE a=5 OR b=7 OR (c=11 AND d=13)
    **
    ** ����������У���3����������terms��OR���ӡ�
    ** ѭ���Ķ���������:
    **
    **          Null       1                # �ڼĴ���1�а�rowset����
    **
    ** ��ô�����ڽ�������ÿ����������term.���ݸ�RowSetTest�Ĳ����ǵ�ǰ���뵽RowSet���е�rowid. 
    ** ����Ѿ����ڣ������������Gosub�����벢��ֱ��������WhereEnd()ֱ�����ɵĴ��롣
    **
    **        sqlite3WhereBegin(<term>)
    **          RowSetTest                  # ��rowid���뵽rowset
    **          Gosub      2 A
    **        sqlite3WhereEnd()
    **
    ** ��������������ֹѭ���Ĵ��롣Label A, ����Gosub��Ŀ��, ��Goto֮��������Ӧ��ָ�
    **          Null       1                # �ڼĴ���1�а�rowset����
    **          Goto       B                # ѭ������.
    **
    **       A: <loop body>                 # Return data, whatever.
    **
    **          Return     2                # ���ص�Gosub
    **
    **       B: <after the loop>
    **
    */
<<<<<<< HEAD
    WhereClause *pOrWc;    /* The OR-clause broken out into subterms */
    SrcList *pOrTab;       /* Shortened table list or OR-clause generation */
    Index *pCov = 0;             /* ���ܸ�������(����Ϊ��) */
    int iCovCur = pParse->nTab++;  /* �α���������ɨ��(����еĻ�) */

    int regReturn = ++pParse->nMem;           /* �Ĵ���ʹ��OP_Gosub*/
    int regRowset = 0;                        /* �Ĵ�RowSet���� */
    int regRowid = 0;                         /* �Ĵ������rowid*/
    int iLoopBody = sqlite3VdbeMakeLabel(v);  /* ѭ���忪ʼ */
    int iRetInit;                             /* regReturn�ĵ�ַ */
    int untestedTerms = 0;             /* һЩ�Ӿ䲻����ȫ���� */
    int ii;                            /* ѭ�������� */
    Expr *pAndExpr = 0;                /* An ".. AND (...)" expression */
=======
    WhereClause *pOrWc;    /* The OR-clause broken out into subterms ORi�Ӿ���ѵ���terms */
    SrcList *pOrTab;       /* Shortened table list or OR-clause generation ��С���б��OR�Ӿ������ */
    Index *pCov = 0;             /* Potential covering index (or NULL) ���ܵĸ�������(��NULL) */
    int iCovCur = pParse->nTab++;  /* Cursor used for index scans (if any) ��������ɨ����α� */

    int regReturn = ++pParse->nMem;           /* Register used with OP_Gosub �Ĵ���ʹ��OP_Gosub */
    int regRowset = 0;                        /* Register for RowSet object ����RowSet����ļĴ��� */
    int regRowid = 0;                         /* Register holding rowid ����rowid�ļĴ��� */
    int iLoopBody = sqlite3VdbeMakeLabel(v);  /* Start of loop body ѭ����Ŀ�ʼ */
    int iRetInit;                             /* Address of regReturn init regReturn��ַ��ʼ�� */
    int untestedTerms = 0;             /* Some terms not completely tested һЩû��ȫ���Ե�terms */
    int ii;                            /* Loop counter ѭ�������� */
    Expr *pAndExpr = 0;                /* An ".. AND (...)" expression һ��".. AND (...)"���ʽ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
   
    pTerm = pLevel->plan.u.pTerm;
    assert( pTerm!=0 );
    assert( pTerm->eOperator==WO_OR );
    assert( (pTerm->wtFlags & TERM_ORINFO)!=0 );
    pOrWc = &pTerm->u.pOrInfo->wc;
    pLevel->op = OP_Return;
    pLevel->p1 = regReturn;

    /* Set up a new SrcList in pOrTab containing the table being scanned
    ** by this loop in the a[0] slot and all notReady tables in a[1..] slots.
    ** This becomes the SrcList in the recursive call to sqlite3WhereBegin().
<<<<<<< HEAD
    ����һ���µ�SrcList pOrTab�����������ͨ��a[0]��a[1..]����ɨ�衣
    �����SrcList�ݹ����sqlite3WhereBegin()��
    */
    if( pWInfo->nLevel>1 ){
      int nNotReady;                 /* notReady �����Ŀ */
      struct SrcList_item *origSrc;     /* ԭʼ�ı��б� */
=======
    **
    ** ��pOrTab�д����µ�SrcList�����SrcList����ͨ�����ѭ��ɨ�赽��a[0]λ�õı����a[1..]λ������notReady�ı�
    */
    if( pWInfo->nLevel>1 ){
      int nNotReady;                 /* The number of notReady tables notReady�����Ŀ */
      struct SrcList_item *origSrc;     /* Original list of tables ���ԭʼ�б� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
      nNotReady = pWInfo->nLevel - iLevel - 1;
      pOrTab = sqlite3StackAllocRaw(pParse->db,
                            sizeof(*pOrTab)+ nNotReady*sizeof(pOrTab->a[0]));
      if( pOrTab==0 ) return notReady;
      pOrTab->nAlloc = (i16)(nNotReady + 1);
      pOrTab->nSrc = pOrTab->nAlloc;
      memcpy(pOrTab->a, pTabItem, sizeof(*pTabItem));
      origSrc = pWInfo->pTabList->a;
      for(k=1; k<=nNotReady; k++){
        memcpy(&pOrTab->a[k], &origSrc[pLevel[k].iFrom], sizeof(pOrTab->a[k]));
      }
    }else{
      pOrTab = pWInfo->pTabList;
    }

    /* Initialize the rowset register to contain NULL. An SQL NULL is 
    ** equivalent to an empty rowset.
<<<<<<< HEAD
    **��ʼ���м��Ĵ�������NULL��һ��SQL���൱��һ�����м���
=======
    **
    ** ��ʼ��rowset�Ĵ�������NULL.һ��SQL NULL�ǵȼ���һ���յ�rowset.
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** Also initialize regReturn to contain the address of the instruction 
    ** immediately following the OP_Return at the bottom of the loop. This
    ** is required in a few obscure LEFT JOIN cases where control jumps
    ** over the top of the loop into the body of it. In this case the 
    ** correct response for the end-of-loop code (the OP_Return) is to 
    ** fall through to the next instruction, just as an OP_Next does if
    ** called on an uninitialized cursor.
<<<<<<< HEAD
    ��ʼ��regReturn����ָ���ַ��OP_Return�ײ�ѭ����
    ������Ҫ�ڼ���ģ������������ڿ�������ѭ���Ķ�������������¡�
    �������������ȷ����Ӧend-of-loop����(OP_Return)�½�����һ��ָ��,
    ����һ��OP_Next�������Ҫ��δ��ʼ����ָ�롣
=======
    **
    ** ͬʱҲ��ʼ��regReturn����ָ���ַ�����regReturn�ǽ�����ѭ���ײ���OP_Return.
    ** �������������ѭ�������������ȷ��Ӧ��Ҫ������һ��ָ�
    ** ������һ��Ϊ��ʼ�����α��е���һ��OP_Next����
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( (wctrlFlags & WHERE_DUPLICATES_OK)==0 ){
      regRowset = ++pParse->nMem;
      regRowid = ++pParse->nMem;
      sqlite3VdbeAddOp2(v, OP_Null, 0, regRowset);
    }
    iRetInit = sqlite3VdbeAddOp2(v, OP_Integer, 0, regReturn);

    /* If the original WHERE clause is z of the form:  (x1 OR x2 OR ...) AND y
    ** Then for every term xN, evaluate as the subexpression: xN AND z
    ** That way, terms in y that are factored into the disjunction will
    ** be picked up by the recursive calls to sqlite3WhereBegin() below.
<<<<<<< HEAD
    **���ԭʼWHERE�Ӿ��z��ʽ:(x1��x2��)��y��Ȼ��ÿ���Ӿ�xN,
    �����ӱ��ʽ:xN��z,�Ӿ��y�ֽ����ȡ�����ݹ����sqlite3WhereBegin()��
=======
    **
    ** ���ԭʼ��WHERE�Ӿ���z������ʽ:(x1 OR x2 OR ...) AND y.��ô����ÿһ��term xN,�����ֱ��ʽ:xN AND z.
    ** ��������y�е�termsͨ�����������sqlite3WhereBegin()�����зֽ⡣
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** Actually, each subexpression is converted to "xN AND w" where w is
    ** the "interesting" terms of z - terms that did not originate in the
    ** ON or USING clause of a LEFT JOIN, and terms that are usable as 
    ** indices.
<<<<<<< HEAD
    ʵ����,ÿ���ӱ��ʽת��Ϊ��xN��w��,w�ǡ���Ȥ����z -
    ���������Դ�������ӻ�ʹ������,�������������
=======
    **
    ** ��ʵ�ϣ�ÿ���ӱ��ʽת��Ϊ"xN AND w"��
    ** ����w��z��"interesting" terms-terms����Դ��LEFT JOIN��ON��USING�Ӿ�,����terms���Ա���������ʹ�á�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( pWC->nTerm>1 ){
      int iTerm;
      for(iTerm=0; iTerm<pWC->nTerm; iTerm++){
        Expr *pExpr = pWC->a[iTerm].pExpr;
        if( ExprHasProperty(pExpr, EP_FromJoin) ) continue;
        if( pWC->a[iTerm].wtFlags & (TERM_VIRTUAL|TERM_ORINFO) ) continue;
        if( (pWC->a[iTerm].eOperator & WO_ALL)==0 ) continue;
        pExpr = sqlite3ExprDup(pParse->db, pExpr, 0);
        pAndExpr = sqlite3ExprAnd(pParse->db, pAndExpr, pExpr);
      }
      if( pAndExpr ){
        pAndExpr = sqlite3PExpr(pParse, TK_AND, 0, pAndExpr, 0);
      }
    }

    for(ii=0; ii<pOrWc->nTerm; ii++){
      WhereTerm *pOrTerm = &pOrWc->a[ii];
      if( pOrTerm->leftCursor==iCur || pOrTerm->eOperator==WO_AND ){
        WhereInfo *pSubWInfo;          /* Info for single OR-term scan OR-termɨ�����Ϣ */
        Expr *pOrExpr = pOrTerm->pExpr;
        if( pAndExpr ){
          pAndExpr->pLeft = pOrExpr;
          pOrExpr = pAndExpr;
        }
<<<<<<< HEAD
        /* Loop through table entries that match term pOrTerm.
        ������������pOrTerm��ƥ�����Ŀ��
         */
=======
        /* Loop through table entries that match term pOrTerm. ѭ����������ƥ��pOrTerm����Ŀ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        pSubWInfo = sqlite3WhereBegin(pParse, pOrTab, pOrExpr, 0, 0,
                        WHERE_OMIT_OPEN_CLOSE | WHERE_AND_ONLY |
                        WHERE_FORCE_TABLE | WHERE_ONETABLE_ONLY, iCovCur);
        assert( pSubWInfo || pParse->nErr || pParse->db->mallocFailed );
        if( pSubWInfo ){
          WhereLevel *pLvl;
          explainOneScan(
              pParse, pOrTab, &pSubWInfo->a[0], iLevel, pLevel->iFrom, 0
          );
          if( (wctrlFlags & WHERE_DUPLICATES_OK)==0 ){
            int iSet = ((ii==pOrWc->nTerm-1)?-1:ii);
            int r;
            r = sqlite3ExprCodeGetColumn(pParse, pTabItem->pTab, -1, iCur, 
                                         regRowid, 0);
            sqlite3VdbeAddOp4Int(v, OP_RowSetTest, regRowset,
                                 sqlite3VdbeCurrentAddr(v)+2, r, iSet);
          }
          sqlite3VdbeAddOp2(v, OP_Gosub, regReturn, iLoopBody);

          /* The pSubWInfo->untestedTerms flag means that this OR term
          ** contained one or more AND term from a notReady table.  The
          ** terms from the notReady table could not be tested and will
          ** need to be tested later.
<<<<<<< HEAD
          pSubWInfo - > untestedTerms��־��ζ����OR���Ӵʰ���һ�������ؼ���
          notReady ���ܲ���,�Ժ���Ҫ���ԡ�
=======
          **
          ** pSubWInfo->untestedTerms��־�������OR term����������һ��notReady���һ������AND term.
          ** ����notReady���terms���ܱ����Բ����Ժ���Ҫ���ԡ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          */
          if( pSubWInfo->untestedTerms ) untestedTerms = 1;

          /* If all of the OR-connected terms are optimized using the same
          ** index, and the index is opened using the same cursor number
          ** by each call to sqlite3WhereBegin() made by this loop, it may
          ** be possible to use that index as a covering index.
<<<<<<< HEAD
          **������е�OR���Ӵʶ����Ѿ��Ż��Ķ���ʹ����ͬ��ָ��,
          ����ʹ����ͬ���α����������ÿ������sqlite3WhereBegin()�����ѭ��,
          �����ܻ�ʹ�ø�ָ����Ϊ����������
=======
          **
          ** ���ʹ����ͬ�������Ż�����OR���ӵ�terms��
          ** ����ͨ��ÿ�ε�����ѭ�������sqlite3WhereBegin()��ʹ����ͬ���α����ݴ�����,
          ** ��ô���ܻ�����������Ϊ��������ʹ�á�
          **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          ** If the call to sqlite3WhereBegin() above resulted in a scan that
          ** uses an index, and this is either the first OR-connected term
          ** processed or the index is the same as that used by all previous
          ** terms, set pCov to the candidate covering index. Otherwise, set 
          ** pCov to NULL to indicate that no candidate covering index will 
          ** be available.
<<<<<<< HEAD
          �������sqlite3WhereBegin()����һ��ʹ������ɨ��,
          ���ǵ�һ��OR���Ӵʴ����ʹ�õ�������һ����,����pCov��ѡ�������� ��
          ����,pCov����Ϊ��,��ʾû�к�ѡ�����������ǿ��õ�
=======
          **
          ** �������sqlite3WhereBegin()����ʹ������ɨ�裬
          ** �������ǵ�һ��ִ��OR-connected term��ʹ�õ�������ǰ�����е�terms��̬,��pCov����Ϊ��ѡ�ĸ���������
          ** ���򣬰�pCov����ΪNULL��˵��û�к�ѡ�ĸ��������ɹ�ʹ�á�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          */
          pLvl = &pSubWInfo->a[0];
          if( (pLvl->plan.wsFlags & WHERE_INDEXED)!=0
           && (pLvl->plan.wsFlags & WHERE_TEMP_INDEX)==0
           && (ii==0 || pLvl->plan.u.pIdx==pCov)
          ){
            assert( pLvl->iIdxCur==iCovCur );
            pCov = pLvl->plan.u.pIdx;
          }else{
            pCov = 0;
          }

<<<<<<< HEAD
          /* Finish the loop through table entries that match term pOrTerm.
          ��ɱ�����������pOrTerm��ƥ�����Ŀ��
           */
=======
          /* Finish the loop through table entries that match term pOrTerm. ��ɱ���ƥ��pOrTerm����Ŀ��ѭ������ */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
          sqlite3WhereEnd(pSubWInfo);
        }
      }
    }
    pLevel->u.pCovidx = pCov;
    if( pCov ) pLevel->iIdxCur = iCovCur;
    if( pAndExpr ){
      pAndExpr->pLeft = 0;
      sqlite3ExprDelete(pParse->db, pAndExpr);
    }
    sqlite3VdbeChangeP1(v, iRetInit, sqlite3VdbeCurrentAddr(v));
    sqlite3VdbeAddOp2(v, OP_Goto, 0, pLevel->addrBrk);
    sqlite3VdbeResolveLabel(v, iLoopBody);

    if( pWInfo->nLevel>1 ) sqlite3StackFree(pParse->db, pOrTab);
    if( !untestedTerms ) disableTerm(pLevel, pTerm);
  }else
#endif /* SQLITE_OMIT_OR_OPTIMIZATION */

  {
    /* Case 5:  There is no usable index.  We must do a complete
    **          scan of the entire table.
<<<<<<< HEAD
    û�п��õ����������Ǳ�����һ��������ɨ��������
=======
    **
    ** ���5:û�п��õ�������������һ��ȫ��ɨ�衣
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    static const u8 aStep[] = { OP_Next, OP_Prev };
    static const u8 aStart[] = { OP_Rewind, OP_Last };
    assert( bRev==0 || bRev==1 );
    assert( omitTable==0 );
    pLevel->op = aStep[bRev];
    pLevel->p1 = iCur;
    pLevel->p2 = 1 + sqlite3VdbeAddOp2(v, aStart[bRev], iCur, addrBrk);
    pLevel->p5 = SQLITE_STMTSTATUS_FULLSCAN_STEP;
  }
  notReady &= ~getMask(pWC->pMaskSet, iCur);

  /* Insert code to test every subexpression that can be completely
  ** computed using the current set of tables.
<<<<<<< HEAD
  **ʹ�õ�ǰ��һ�������ȫ������������Լ���ÿ���ӱ��ʽ��
  ** IMPLEMENTATION-OF: R-49525-50935 Terms that cannot be satisfied through
  ** the use of indices become tests that are evaluated against each row of
  ** the relevant input tables.
  ������������ͨ��ʹ��ָ����Ϊ����������ص�������ÿһ�С�
=======
  **
  ** �������������ÿһ������ʹ�õ�ǰһϵ�еı�������������ӱ��ʽ
  **
  ** IMPLEMENTATION-OF: R-49525-50935 Terms that cannot be satisfied through
  ** the use of indices become tests that are evaluated against each row of
  ** the relevant input tables.
  ** IMPLEMENTATION-OF: R-49525-50935 ����ʹ��������terms��Ϊ�������������ÿ�еĲ���
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  for(pTerm=pWC->a, j=pWC->nTerm; j>0; j--, pTerm++){
    Expr *pE;
    testcase( pTerm->wtFlags & TERM_VIRTUAL ); /* IMP: R-30575-11662 */
    testcase( pTerm->wtFlags & TERM_CODED );
    if( pTerm->wtFlags & (TERM_VIRTUAL|TERM_CODED) ) continue;
    if( (pTerm->prereqAll & notReady)!=0 ){
      testcase( pWInfo->untestedTerms==0
               && (pWInfo->wctrlFlags & WHERE_ONETABLE_ONLY)!=0 );
      pWInfo->untestedTerms = 1;
      continue;
    }
    pE = pTerm->pExpr;
    assert( pE!=0 );
    if( pLevel->iLeftJoin && !ExprHasProperty(pE, EP_FromJoin) ){
      continue;
    }
    sqlite3ExprIfFalse(pParse, pE, addrCont, SQLITE_JUMPIFNULL);
    pTerm->wtFlags |= TERM_CODED;
  }

  /* For a LEFT OUTER JOIN, generate code that will record the fact that
<<<<<<< HEAD
  ** at least one row of the right table has matched the left table.  
  ����һ����������,���ɵĴ��뽫��¼����ʵ���ٶԱ��һ��ƥ�����
=======
  ** at least one row of the right table has matched the left table. 
  **
  ** ����һ��LEFT OUTER JOIN,���ɴ�������¼�ұ�������һ���������ƥ�����ʵ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pLevel->iLeftJoin ){
    pLevel->addrFirst = sqlite3VdbeCurrentAddr(v);
    sqlite3VdbeAddOp2(v, OP_Integer, 1, pLevel->iLeftJoin);
    VdbeComment((v, "record LEFT JOIN hit"));
    sqlite3ExprCacheClear(pParse);
    for(pTerm=pWC->a, j=0; j<pWC->nTerm; j++, pTerm++){
      testcase( pTerm->wtFlags & TERM_VIRTUAL );  /* IMP: R-30575-11662 */
      testcase( pTerm->wtFlags & TERM_CODED );
      if( pTerm->wtFlags & (TERM_VIRTUAL|TERM_CODED) ) continue;
      if( (pTerm->prereqAll & notReady)!=0 ){
        assert( pWInfo->untestedTerms );
        continue;
      }
      assert( pTerm->pExpr );
      sqlite3ExprIfFalse(pParse, pTerm->pExpr, addrCont, SQLITE_JUMPIFNULL);
      pTerm->wtFlags |= TERM_CODED;
    }
  }
  sqlite3ReleaseTempReg(pParse, iReleaseReg);

  return notReady;
}

#if defined(SQLITE_TEST)
/*
** The following variable holds a text description of query plan generated
** by the most recent call to sqlite3WhereBegin().  Each call to WhereBegin
** overwrites the previous.  This information is used for testing and
** analysis only.
<<<<<<< HEAD
���±���������ı��������ɲ�ѯ�ƻ�����
ͨ������ĵ���qlite3WhereBegin()��
ÿ������WhereBegin����ǰ��ġ�
��Щ��Ϣ�����ڲ��Ժͷ�����
*/
char sqlite3_query_plan[BMS*2*40];  /*�����ı� */
static int nQPlan = 0;              /*�ͷ���һ��in _query_plan[] */
//���㳬 �Ӵ˽���
=======
**
** ����ı�������һ������ͨ�����µĵ���sqlite3WhereBegin()���ɵĲ�ѯ�ƻ����ı���
** ÿ�ε���WhereBegin��д��ǰ����Ϣ�������Ϣֻ���ڲ��Ժͷ�����
*/
char sqlite3_query_plan[BMS*2*40];  /* Text of the join ���ӵ����� */
static int nQPlan = 0;              /* Next free slow in _query_plan[] */

>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
#endif /* SQLITE_TEST */


/*

** Free a WhereInfo structure
**
** �ͷ�һ��WhereInfo���ݽṹ
*/
static void whereInfoFree(sqlite3 *db, WhereInfo *pWInfo){
  if( ALWAYS(pWInfo) ){
    int i;
    for(i=0; i<pWInfo->nLevel; i++){
      sqlite3_index_info *pInfo = pWInfo->a[i].pIdxInfo;
      if( pInfo ){
        /* assert( pInfo->needToFreeIdxStr==0 || db->mallocFailed ); */
        if( pInfo->needToFreeIdxStr ){
          sqlite3_free(pInfo->idxStr);
        }
        sqlite3DbFree(db, pInfo);
      }
      if( pWInfo->a[i].plan.wsFlags & WHERE_TEMP_INDEX ){
        Index *pIdx = pWInfo->a[i].plan.u.pIdx;
        if( pIdx ){
          sqlite3DbFree(db, pIdx->zColAff);
          sqlite3DbFree(db, pIdx);
        }
      }
    }
    whereClauseClear(pWInfo->pWC);
    sqlite3DbFree(db, pWInfo);
  }
}


/*
** Generate the beginning of the loop used for WHERE clause processing.
** The return value is a pointer to an opaque structure that contains
** information needed to terminate the loop.  Later, the calling routine
** should invoke sqlite3WhereEnd() with the return value of this function
** in order to complete the WHERE clause processing.
��������ѭ���Ŀ�ʼWHERE�Ӿ乤�ռӹ��ķ���ֵ��һ��ָ��һ����͸���Ľṹ��������
?�������Ϣ������ֹ��ѭ������������������Ӧ��Ϊ�����WHERE�Ӿ䴦�����sqlite3WhereEnd������������ķ���ֵ��
**
** ����ѭ���Ŀ�ʼ����WHERE�Ӿ�Ĵ���
** ����ֵ��һ��ָ��,��ָ��һ��������ֹѭ���������Ϣ�Ĳ�͸���Ľṹ�塣
** �Ժ󣬵��ó���������������ķ���ֵ����sqlite3WhereEnd()�����WHERE�Ӿ�Ĵ���
**
** If an error occurs, this routine returns NULL.
������ִ���������̷���null��
**
** �����������������򽫷���NULL.
**
** The basic idea is to do a nested loop, one loop for each table in
** the FROM clause of a select.  (INSERT and UPDATE statements are the
** same as a SELECT with only a single table in the FROM clause.)  For
** example, if the SQL is this:
�����˼·����һ��Ƕ�׵�ѭ����һ��ѭ����ÿ�����һ��ѡ����
��INSERT��UPDATE�����ͬ��SELECT���ڽ�������FROM�Ӿ䣩��
���磬���SQL�������ģ�
**
**       SELECT * FROM t1, t2, t3 WHERE ...;
**
** Then the code generated is conceptually like the following:
**
**      foreach row1 in t1 do       \    Code generated
**        foreach row2 in t2 do      |-- by sqlite3WhereBegin()
**          foreach row3 in t3 do   /
**            ...
**          end                     \    Code generated
**        end                        |-- by sqlite3WhereEnd()
**      end                         /
**
**
** ������˼·�ǽ���ѭ��Ƕ�ף�Ϊһ����ѯ����FROM�Ӿ��е�ÿ������һ��ѭ����
** (INSERT��UPDATE��������FROM�����ֻ��һ�����SELECT��ͬ)������:���SQL��:
**       SELECT * FROM t1, t2, t3 WHERE ...;
** ��ô�������������´���:
**      foreach row1 in t1 do       \
**        foreach row2 in t2 do      |-- ��sqlite3WhereBegin()����
**          foreach row3 in t3 do   /
**            ...
**          end                     \
**        end                        |-- ��sqlite3WhereEnd()����
**      end                         /
**
**
** Note that the loops might not be nested in the order in which they
** appear in the FROM clause if a different order is better able to make
** use of indices.  Note also that when the IN operator appears in
** the WHERE clause, it might result in additional nested loops for
** scanning through all values on the right-hand side of the IN.
ע�⣬�û����ܲ��ᱻǶ�������ǳ�����FROM�Ӿ�����Բ�ͬ��˳����
�ܹ����õ�����������˳�򡣻�Ҫע����ǣ����ڲ�������ʾ��WHERE�Ӿ��У�
�����ܻᵼ��͸����IN�����ֲ������ֵ�ĸ��ӵ�Ƕ��ѭ������ɨ�衣
**
** ע��:ѭ�����ܲ��ǰ�FROM�Ӿ������ǳ��ֵ�˳�����Ƕ�ף���Ϊ����һ��������Ƕ��˳����ʺ�ʹ��������
** ��Ҫע��;��WHERE�Ӿ��г�����IN�������������ܵ������Ƕ��ѭ����ɨ��IN�ұߵ�����ֵ��
**
** There are Btree cursors associated with each table.  t1 uses cursor
** number pTabList->a[0].iCursor.  t2 uses the cursor pTabList->a[1].iCursor.
** And so forth.  This routine generates code to open those VDBE cursors
** and sqlite3WhereEnd() generates the code to close them.
����ÿ�����������B���αꡣT1ʹ�ù���pTabList->һ��[0].iCursor��
T2ʹ�ù��pTabList->һ[1].iCursor��
����������ɴ���������ЩVDBE����sqlite3WhereEnd�������ɵĴ������ر����ǡ�
**
** ��Btree�α���ÿ�����������t1ʹ���α���pTabList->a[0].iCursor.t2ʹ���α�pTabList->a[1].iCursor.�ȵ�
** ����������ɴ���������ЩVDBE�α꣬sqlite3WhereEnd()���ɴ������ر����ǡ�
**
** The code that sqlite3WhereBegin() generates leaves the cursors named
** in pTabList pointing at their appropriate entries.  The [...] code
** can use OP_Column and OP_Rowid opcodes on these cursors to extract
** data from the various tables of the loop.
��sqlite3WhereBegin�������ɵĴ���ҶpTabList����ָ���Լ�����
Ӧ��Ŀ�Ĺ�ꡣ��[...]�Ĵ������ʹ��OP_Column��OP_Rowid������
����Щ���ӻ�·�ĸ��ֱ�����ȡ����
**
** sqlite3WhereBegin()���ɵĴ�����pTabList������ָ�����α�ָ������ǡ������Ŀ��
** [...]�������ʹ������Щ�α��е�OP_Column��OP_Rowid����������ѭ���ĸ���������ȡ���ݡ�
**
** If the WHERE clause is empty, the foreach loops must each scan their
** entire tables.  Thus a three-way join is an O(N^3) operation.  But if
** the tables have indices and there are terms in the WHERE clause that
** refer to those indices, a complete table scan can be avoided and the
** code will run much faster.  Most of the work of this routine is checking
** to see if there are indices that can be used to speed up the loop.
<<<<<<< HEAD
**���WHERE�Ӿ��ǿյģ�foreachѭ��������ÿ��ɨ�����ǵ�������
��ˣ�һ����·������O��N^3�����������ǣ������������������
WHERE������ָ��Щ������һ�������ı�ɨ�裬�ɱ���ʹ��뽫
���еø��졣�󲿷ָó���Ĺ����Ǽ��
�Բ鿴�Ƿ�������������������ѭ����
=======
**
** ���WHERE�Ӿ��ǿյģ�ÿһ��ѭ������ÿ��ɨ�����������һ��3��������һ��O(N^3)������
** ���������������������WHERE�Ӿ�����terms����Щ�����������һ�����Ա��������ı�ɨ�貢�Ҵ������еĸ��졣
** �������󲿷ֵĹ����Ǽ���Ƿ��п���ʹ�õ�����������ѭ����
**
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
** Terms of the WHERE clause are also used to limit which rows actually
** make it to the "..." in the middle of the loop.  After each "foreach",
** terms of the WHERE clause that use only terms in that loop and outer
** loops are evaluated and if false a jump is made around all subsequent
** inner loops (or around the "..." if the test occurs within the inner-
** most loop)
��WHER��Ҳ����������Щ��ʵ����ʹ���ġ�...������ѭ�����м䡣
ÿһ������foreach����ʹ����ѭ������ѭ��ֻ����WHERE�Ӿ��е���
�����������������תΧ�����к����ڻ�ȡ�ã���Χ�ơ�......�����
�����г��ֵ�inner-���·��

**
** WHERE�Ӿ��termsҲ������������ѭ�����в���Щ��ʹ����Ϊ"...".
** ÿ��ѭ����WHERE�Ӿ��termsֻʹ�����Ǹ�ѭ�����ⲿѭ����������terms.
** �������������������к������ڲ�ѭ��(��������Է��������ڲ�ѭ���У���ô������"...")
**
** OUTER JOINS
**
** An outer join of tables t1 and t2 is conceptally coded as follows:
**
**    foreach row1 in t1 do
**      flag = 0
**      foreach row2 in t2 do
**        start:
**          ...
**          flag = 1
**      end
**      if flag==0 then
**        move the row2 cursor to a null row
**        goto start
**      fi
**    end
**
**
** OUTER JOINS
** һ����t1��t2���ⲿ���ӻ��ڸ������������´���:
**    foreach row1 in t1 do
**      flag = 0
**      foreach row2 in t2 do
**        start:
**          ...
**          flag = 1
**      end
**      if flag==0 then
**        move the row2 cursor to a null row
**        goto start
**      fi
**    end
**
**
** ORDER BY CLAUSE PROCESSING
ORDER BY������
**
** *ppOrderBy is a pointer to the ORDER BY clause of a SELECT statement,
** if there is one.  If there is no ORDER BY clause or if this routine
** is called from an UPDATE or DELETE statement, then ppOrderBy is NULL.
**ppOrderBy��һ��ָ��ORDER BY��SELECT����WHERE����
�����һ�������û��ORDER BY�Ӿ䣬��������������
��Ϊ��UPDATE��DELETE��䣬Ȼ��ppOrderByΪNULL��
** If an index can be used so that the natural output order of the table
** scan is correct for the ORDER BY clause, then that index is used and
** *ppOrderBy is set to NULL.  This is an optimization that prevents an
** unnecessary sort of the result set if an index appropriate for the
** ORDER BY clause already exists.
���һ��������������ʹ��ɨ�����Ȼ���˳������ȷ��ORDER BY����
���������ʹ�ú�ppOrderBy����ΪNULL��������ֹ�Ľ��������Ѿ���
�ڵ�ָ���ʺ���ORDER BY�Ӿ������õĲ���Ҫ�������Ż�..
**
** If the where clause loops cannot be arranged to provide the correct
** output order, then the *ppOrderBy is unchanged.
<<<<<<< HEAD
���where�Ӿ�ѭ���޷������ṩ��ȷ��
���˳����ô* ppOrderBy���䡣
*/
WhereInfo *sqlite3WhereBegin(
  Parse *pParse,        /* The parser context  �������Ļ���*/
  SrcList *pTabList,    /* A list of all tables to be scanned  Ҫɨ������б���б�*/
  Expr *pWhere,         /* The WHERE clause  WHERE��*/
  ExprList **ppOrderBy, /* An ORDER BY clause, or NULL  ORDER BY������NULL*/
  ExprList *pDistinct,  /* The select-list for DISTINCT queries - or NULL  ѡ���б��е�DISTINCT��ѯ - ��NULL*/
  u16 wctrlFlags,       /* One of the WHERE_* flags defined in sqliteInt.h  һ����sqliteInt.h�����WHERE_*��־ */
  int iIdxCur           /* If WHERE_ONETABLE_ONLY is set, index cursor number ���WHERE_ONETABLE_ONLY�����ã���������*/
){
  int i;                     /* Loop counter  ѭ��������*/
  int nByteWInfo;            /* Num. bytes allocated for WhereInfo struct �����WhereInfo�ṹ�ֽ�*/
  int nTabList;              /* Number of elements in pTabList  ��pTabListԪ�ص�����*/
  WhereInfo *pWInfo;         /* Will become the return value of this function  ����Ϊ�ú����ķ���ֵ*/
  Vdbe *v = pParse->pVdbe;   /* The virtual database engine  �������ݿ�����*/
  Bitmask notReady;          /* Cursors that are not yet positioned  ��Щ��δ��λ�α�*/
  WhereMaskSet *pMaskSet;    /* The expression mask set  ���mask��*/
  WhereClause *pWC;               /* Decomposition of the WHERE clause  WHERE���ֽ�*/
  struct SrcList_item *pTabItem;  /* A single entry from pTabList  ��pTabList������Ŀ*/
  WhereLevel *pLevel;             /* A single level in the pWInfo list  ��pWInfo�б��еĵ���ˮƽ*/
  int iFrom;                      /* First unused FROM clause element ��һ��δʹ��FROM�Ӿ��е�Ԫ�� */
  int andFlags;              /* AND-ed combination of all pWC->a[].wtFlags */
  sqlite3 *db;               /* Database connection  ���ݿ�����*/

  /* The number of tables in the FROM clause is limited by the number of
  ** bits in a Bitmask 
  ��from�Ӽ��б��������bitmask�еı�������
=======
**
** ORDER BY�Ӿ䴦��
** �����ORDER BY�Ӿ䣬��ô*ppOrderBy��һ��ָ�룬ָ��һ��SELECT�����ORDER BY�Ӿ䡣
** ���û��ORDER BY�Ӿ����UPDATE��DELETE���õ����������ôppOrderByΪNULL.
** �������ʹ�������Ա�ɨ�����Ȼ���˳���Ǹ���ORDER BY�Ӿ�����ģ���ôʹ����������*ppOrderBy����ΪNULL.
** ���һ��������ORDER BY�Ӿ�������Ѿ����ڣ���һ����ֹ���������Ҫ��������Ż�
** ������ܰ���WHERE�Ӿ�ѭ���ṩ��ȷ�����˳����ô���ı�*ppOrderBy��ֵ��
**
*/
WhereInfo *sqlite3WhereBegin(
  Parse *pParse,        /* The parser context ���������� */
  SrcList *pTabList,    /* A list of all tables to be scanned ��ɨ������б��һ���б� */
  Expr *pWhere,         /* The WHERE clause WHERE�Ӿ� */
  ExprList **ppOrderBy, /* An ORDER BY clause, or NULL һ��ORDER BY�Ӿ��NULL*/
  ExprList *pDistinct,  /* The select-list for DISTINCT queries - or NULL DISTINCT��ѯ�Ĳ�ѯ�б��NULL */
  u16 wctrlFlags,       /* One of the WHERE_* flags defined in sqliteInt.h ��sqliteInt.h�ж����WHERE_*�е�һ�� */
  int iIdxCur           /* If WHERE_ONETABLE_ONLY is set, index cursor number ���������WHERE_ONETABLE_ONLY����Ϊ�����α��� */
){
  int i;                     /* Loop counter ѭ�������� */
  int nByteWInfo;            /* Num. bytes allocated for WhereInfo struct ΪWhereInfo�ṹ������ֽ��� */
  int nTabList;              /* Number of elements in pTabList ��pTabList�е�Ԫ���� */
  WhereInfo *pWInfo;         /* Will become the return value of this function �������������ķ���ֵ */
  Vdbe *v = pParse->pVdbe;   /* The virtual database engine �������ݿ����� */
  Bitmask notReady;          /* Cursors that are not yet positioned ��δȷ��λ�õ��α� */
  WhereMaskSet *pMaskSet;    /* The expression mask set ���ñ��ʽ���� */
  WhereClause *pWC;               /* Decomposition of the WHERE clause WHERE�Ӿ�ķֽ� */
  struct SrcList_item *pTabItem;  /* A single entry from pTabList ������pTabList��һ����Ŀ */
  WhereLevel *pLevel;             /* A single level in the pWInfo list ��pWInfo�б��е�һ���Ǽ� */
  int iFrom;                      /* First unused FROM clause element ��һ��δʹ�õ�FROM�Ӿ�Ԫ�� */
  int andFlags;              /* AND-ed combination of all pWC->a[].wtFlags AND-ed���е�pWC->a[].wtFlags��� */
  sqlite3 *db;               /* Database connection ���ݿ����� */

  /* The number of tables in the FROM clause is limited by the number of
  ** bits in a Bitmask 
  **
  ** ��һ��λ�����е�λ����������FROM�Ӿ��еı���Ŀ
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  testcase( pTabList->nSrc==BMS );	//���ڸ��ǲ���
  if( pTabList->nSrc>BMS ){	//�����FROM�еı���Ŀ����λ�����е�λ��
    sqlite3ErrorMsg(pParse, "at most %d tables in a join", BMS);	//��ʾ���������ֻ����BMS����
    return 0;
  }

  /* This function normally generates a nested loop for all tables in 
  ** pTabList.  But if the WHERE_ONETABLE_ONLY flag is set, then we should
  ** only generate code for the first table in pTabList and assume that
  ** any cursors associated with subsequent tables are uninitialized.
<<<<<<< HEAD
  �ù���ͨ�������һ��Ƕ��ѭ����pTabList���б�
  �����WHERE_ONETABLE_ONLY��־���ã���ô����Ӧ��ֻ
  ���ɴ���ΪpTabList��һ�������ٶ�������ص��κ��α��ʼ����
=======
  **
  ** �������һ����Ϊ��pTabList�е����б�����һ��Ƕ��ѭ����
  ** �������������WHERE_ONETABLE_ONLY��־��
  ** ��ô����ֻ��ҪΪpTabList�еĵ�һ�������ɴ��벢�Ҽ����κ�������ı���ص��α궼��δ��ʼ���ġ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  nTabList = (wctrlFlags & WHERE_ONETABLE_ONLY) ? 1 : pTabList->nSrc;//���ֻ��һ������ônTabList��Ϊ1���������pTabList->nSrc

  /* Allocate and initialize the WhereInfo structure that will become the
  ** return value. A single allocation is used to store the WhereInfo
  ** struct, the contents of WhereInfo.a[], the WhereClause structure
  ** and the WhereMaskSet structure. Since WhereClause contains an 8-byte
  ** field (type Bitmask) it must be aligned on an 8-byte boundary on
  ** some architectures. Hence the ROUND8() below.
<<<<<<< HEAD
  ����ͳ�ʼ��WhereInfo�ṹ����Ϊ����ֵ��
  �����������ڴ洢����WhereInfo�ṹ��WhereInfo.a[]�У�
  WhereClause�ṹ�����ݺ�WhereMaskSet�ṹ��
  ��ΪWhereClause����һ��8�ֽڵ��ֶΣ�����λ���룩
  �������ĳЩ��ϵ�ṹ��8�ֽڱ߽���롣���ROUND8�������¡�
=======
  **
  ** ����ͳ�ʼ��WhereInfo���ݽṹ����ɷ���ֵ��
  ** һ�������ķ��䱻���ڴ洢WhereInfo�ṹ��WhereInfo.a[]�����ݣ�WhereClause���ݽṹ��WhereMaskSet���ݽṹ��
  ** ��ΪWhereClause����һ��8�ֽڵ��ֶα����ڽṹ����һ��8�ֽڱ߽���롣
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  db = pParse->db;	//�������ݿ�
  nByteWInfo = ROUND8(sizeof(WhereInfo)+(nTabList-1)*sizeof(WhereLevel));	//ΪWhereInfo������Ӧ���ֽ���
  pWInfo = sqlite3DbMallocZero(db, 
      nByteWInfo + 
      sizeof(WhereClause) +
      sizeof(WhereMaskSet)
  );	//ΪWhereInfo����ռ�
  if( db->mallocFailed ){	//������ݿ��ڴ����ʧ��
    sqlite3DbFree(db, pWInfo);	//�ͷ����ݿ����ӵ��ڴ�
    pWInfo = 0;	//��շ���Ŀռ䡣
    goto whereBeginError;	//��ת��whereBeginError�������
  }
  pWInfo->nLevel = nTabList;//ѭ��Ƕ����Ϊ�����Ŀ
  pWInfo->pParse = pParse;
  pWInfo->pTabList = pTabList;	//WhereInfo�еı��б���pTabList��ͬ
  pWInfo->iBreak = sqlite3VdbeMakeLabel(v);	//��ֹѭ���ı�־
  pWInfo->pWC = pWC = (WhereClause *)&((u8 *)pWInfo)[nByteWInfo];
  pWInfo->wctrlFlags = wctrlFlags;
  pWInfo->savedNQueryLoop = pParse->nQueryLoop;	//һ����ѯ�ĵ�����
  pMaskSet = (WhereMaskSet*)&pWC[1];

  /* Disable the DISTINCT optimization if SQLITE_DistinctOpt is set via
  ** sqlite3_test_ctrl(SQLITE_TESTCTRL_OPTIMIZATIONS,...) 
<<<<<<< HEAD
  ����DISTINCT���Ż������SQLITE_DistinctOptͨ��sqlite3_test_ctrl����
  ��SQLITE_TESTCTRL_OPTIMIZATIONS��...��*/
=======
  **
  ** ���ͨ��sqlite3_test_ctrl(SQLITE_TESTCTRL_OPTIMIZATIONS,...)����SQLITE_DistinctOpt��ô�ͽ���DISTINCT�Ż�
  */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  if( db->flags & SQLITE_DistinctOpt ) pDistinct = 0;

  /* Split the WHERE clause into separate subexpressions where each
  ** subexpression is separated by an AND operator.
<<<<<<< HEAD

  ����WHERE���ɶ����ļ����ʽ������ÿ�������ʽ��һ��AND������롣
=======
  **
  ** ��WHERE�Ӿ�ͨ��AND������ָ�ɶ���ӱ��ʽ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  initMaskSet(pMaskSet);	//��ʼ��WhereMaskSet����
  whereClauseInit(pWC, pParse, pMaskSet, wctrlFlags);	//��ʼ��pWC
  sqlite3ExprCodeConstants(pParse, pWhere); //Ԥ�ȼ�����pWhere�еĳ����ֱ��ʽ
  whereSplit(pWC, pWhere, TK_AND);  //��WHERE�Ӿ�ͨ��AND������ָ�ɶ���ӱ��ʽ�� /* IMP: R-15842-53296 */
    
  /* Special case: a WHERE clause that is constant.  Evaluate the
  ** expression and either jump over all of the code or fall thru.
<<<<<<< HEAD
  ���������һ��WHERE�Ӽ��Ǻ㶨�ġ�������ʽ��Ҫô�������еĴ�����½���
=======
  **
  ** �������:һ��WHERE�Ӿ��Ǻ㶨�ġ��Ա��ʽ��ֵʱ��Ҫô�������еĴ��룬Ҫôͨ��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pWhere && (nTabList==0 || sqlite3ExprIsConstantNotJoin(pWhere)) ){
    sqlite3ExprIfFalse(pParse, pWhere, pWInfo->iBreak, SQLITE_JUMPIFNULL);
    pWhere = 0;
  }

  /* Assign a bit from the bitmask to every term in the FROM clause.
<<<<<<< HEAD
  **����һ��λ��������ÿ����FROM�Ӽ���
=======
  **
  ** ��λ�����е�һbit�����FROM�Ӿ��ÿ��term��
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  ** When assigning bitmask values to FROM clause cursors, it must be
  ** the case that if X is the bitmask for the N-th FROM clause term then
  ** the bitmask for all FROM clause terms to the left of the N-th term
  ** is (X-1).   An expression from the ON clause of a LEFT JOIN can use
  ** its Expr.iRightJoinTable value to find the bitmask of the right table
  ** of the join.  Subtracting one from the right table bitmask gives a
  ** bitmask for all tables to the left of the join.  Knowing the bitmask
  ** for all tables to the left of a left join is important.  Ticket #3015.
  ��ָ��λ����ֵFROM�Ӿ��꣬��һ���������ģ�
  ���X��λ����N��FROM�Ӽ��ж��ڵģ�������Ϊ����FROM�Ӽ�������N�����ڵ���
Ϊ��X-1������LEFT��ON�Ӿ���ʽJOIN����ʹ����Expr.iRightJoinTable
��ֵ���ֵļ����ұ�����롣�����ұ�λ�����м�ȥ1������һ��
λ����Ϊ���б�ļ�����ࡣ֪��λ����������б������ӵ����
����Ҫ�ġ�Ʊ��3015
  **
  ** ����λ����ֵ�����FROM�Ӿ��α�ʱ�����X��N-th FROM�Ӿ����λ���룬
  ** ��ô����FROM�Ӿ�terms����ߵĵ�N���λ������(X-1)��
  ** һ��������LEFT JOIN��ON�Ӿ�ı��ʽ����ʹ�����Լ���Expr.iRightJoinTableֵ������������ӵ��ұ��λ���롣
  ** ���ұ߱��λ�����м�ȥһ�������λ��������ӵ���ߵ����еı�
  ** Ҫ֪��һ����������ߵ����б��λ�����Ǻ���Ҫ�ġ�
  **
  ** Configure the WhereClause.vmask variable so that bits that correspond
  ** to virtual table cursors are set. This is used to selectively disable 
  ** the OR-to-IN transformation in exprAnalyzeOrTerm(). It is not helpful
  ** with virtual tables.
  ����WhereClause.vmask�������Ա��Ӧ�����ָ��λ���á�
  ��������ѡ���Եؽ���exprAnalyzeOrTerm�Ļ�IN�任������
  ����������û�а����ġ�
  **
  ** ����WhereClause.vmask�����Ա�bits�����úõ��������α���һ�¡�
  ** ��������exprAnalyzeOrTerm()����ѡ���Եؽ���OR-to-INת�������������ʾ���õġ�
  **
  ** Note that bitmasks are created for all pTabList->nSrc tables in
  ** pTabList, not just the first nTabList tables.  nTabList is normally
  ** equal to pTabList->nSrc but might be shortened to 1 if the
  ** WHERE_ONETABLE_ONLY flag is set.
<<<<<<< HEAD
  ��Ҫע�����λ����Ϊ����pTabList-> NSRC����pTabList��
  ��ֻ�ǵ�һ��nTabList������nTabListͨ������pTabList-> NSRC����������Ϊ1��
  �����WHERE_ONETABLE_ONLY��־�����á�
=======
  **
  ** ע��:��ֻ��Ϊ��һ��nTabList����λ���룬����Ϊ��pTabList�е�����pTabList->nSrc������
  ** nTabListһ���ͬ��pTabList->nSrc�������������WHERE_ONETABLE_ONLY��־����ô����������Ϊ1��
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  assert( pWC->vmask==0 && pMaskSet->n==0 );
  for(i=0; i<pTabList->nSrc; i++){	//������ΪFROM�е�ÿ�����α�iCursor��������
    createMask(pMaskSet, pTabList->a[i].iCursor);
#ifndef SQLITE_OMIT_VIRTUALTABLE
    if( ALWAYS(pTabList->a[i].pTab) && IsVirtual(pTabList->a[i].pTab) ){	//����������
      pWC->vmask |= ((Bitmask)1 << i);	//����ʶ������α�ĵ�λ����
    }
#endif
  }
#ifndef NDEBUG
  {
    Bitmask toTheLeft = 0;
    for(i=0; i<pTabList->nSrc; i++){ //ѭ������FROM�Ӿ��еı���Ӳ�ѯ
      Bitmask m = getMask(pMaskSet, pTabList->a[i].iCursor);
      assert( (m-1)==toTheLeft );
      toTheLeft |= m; //����toTheLeft
    }
  }
#endif

  /* Analyze all of the subexpressions.  Note that exprAnalyze() might
  ** add new virtual terms onto the end of the WHERE clause.  We do not
  ** want to analyze these virtual terms, so start analyzing at the end
  ** and work forward so that the added virtual terms are never processed.
<<<<<<< HEAD
  �������е��ӱ��ʽ����Ҫע�����exprAnalyze�������ܻ�����µ�����
  ������WHERE�Ӿ��ĩβ�����ǲ�ϣ����������Щ���������Կ�ʼ�ڶ�
  �����͹�����ǰ����ʹ�����˴�δ�����������
=======
  **
  ** �������е��ӱ��ʽ��ע��exprAnalyze()��������µ������WHERE�Ӿ��ĩβ��
  ** ���ǲ��������Щ������������ʼ�������ҽ����ʹ�ã����Ա���ӵ��������δ������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  exprAnalyzeAll(pTabList, pWC);//����where�Ӿ��е�����terms
  if( db->mallocFailed ){  //������ݿ��ڴ�������
    goto whereBeginError; //��ת��whereBeginError�������
  }

  /* Check if the DISTINCT qualifier, if there is one, is redundant. 
  ** If it is, then set pDistinct to NULL and WhereInfo.eDistinct to
  ** WHERE_DISTINCT_UNIQUE to tell the caller to ignore the DISTINCT.
<<<<<<< HEAD
  ����Ƿ�DISTINCT�޶����������һ�����Ƕ���ġ�����ǣ�������pDistinctΪNULL��
  WhereInfo.eDistinct��WHERE_DISTINCT_UNIQUE���ߵ����ߺ���DISTINCT��
=======
  **
  ** ���DISTINCT�޶����Ƿ��Ƕ���ġ�����ǣ�
  ** ��ô��pDistinct����ΪNULL���Ұ�WhereInfo.eDistinct����ΪWHERE_DISTINCT_UNIQUE�����ߵ����ߺ���DISTINCT.
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( pDistinct && isDistinctRedundant(pParse, pTabList, pWC, pDistinct) ){ //���Distinct�Ƕ����
    pDistinct = 0; //���pDistinct
    pWInfo->eDistinct = WHERE_DISTINCT_UNIQUE; //����WhereInfo�����ߵ����ߺ���DISTINCT
  }

  /* Chose the best index to use for each table in the FROM clause.
<<<<<<< HEAD
  **ѡ����Ϊÿ����ʹ��FROM�Ӽ��е����������
=======
  **
  ** ��FROM�Ӿ���Ϊÿ����ѡ��ʹ����õ�����
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  ** This loop fills in the following fields:
  ѭ����д�����ֶΣ�
  **
  **   pWInfo->a[].pIdx      The index to use for this level of the loop.��ָ������ѭ�������ˮƽ
  **   pWInfo->a[].wsFlags   WHERE_xxx flags associated with pIdx WHERE_xxx��PIDX��صı�־
  **   pWInfo->a[].nEq       The number of == and IN constraints ==��IN��������
  **   pWInfo->a[].iFrom     Which term of the FROM clause is being coded ���е�FROM�Ӽ��б��������
  **   pWInfo->a[].iTabCur   The VDBE cursor for the database table ��VDBE������ݿ��
  **   pWInfo->a[].iIdxCur   The VDBE cursor for the index ��VDBE�������
  **   pWInfo->a[].pTerm     When wsFlags==WO_OR, the OR-clause term ��wsFlags== WO_ORʱ��OR-��
  **
  ** This loop also figures out the nesting order of tables in the FROM
  ** clause.
<<<<<<< HEAD
  ���ѭ��Ҳ���Լ������FROM�Ӽ��е�Ƕ��˳��
=======
  **
  ** ���ѭ�������������:
  **   pWInfo->a[].pIdx      Ϊѭ���ȼ�ʹ�õ�����
  **   pWInfo->a[].wsFlags   ��pIdx�йص�WHERE_xxx��־
  **   pWInfo->a[].nEq       ==��INԼ������Ŀ
  **   pWInfo->a[].iFrom     �������FROM�Ӿ���
  **   pWInfo->a[].iTabCur   �������ݿ���VDBE�α�
  **   pWInfo->a[].iIdxCur   ����������VDBE�α�
  **   pWInfo->a[].pTerm     ��wsFlags==WO_ORʱ��OR�Ӿ���
  ** ���ѭ��Ҳ�����FROM�Ӿ��еı��Ƕ��˳��
  **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  notReady = ~(Bitmask)0;//~ȡ����
  andFlags = ~0;
  WHERETRACE(("*** Optimizer Start ***\n"));
<<<<<<< HEAD
  for(i=iFrom=0, pLevel=pWInfo->a; i<nTabList; i++, pLevel++){
    WhereCost bestPlan;         /* Most efficient plan seen so far  ���񿴵�����Ч�ļƻ�*/
    Index *pIdx;                /* Index for FROM table at pTabItem  �ӱ���pTabItem����*/
    int j;                      /* For looping over FROM tables  ���ڱ���FROM��*/
    int bestJ = -1;             /* The value of j j��ֵ*/
    Bitmask m;                  /* Bitmask value for j or bestJ  ����J��bestJλ����ֵ*/
    int isOptimal;              /* Iterator for optimal/non-optimal search  �������/���������*/
    int nUnconstrained;         /* Number tables without INDEXED BY  INT nUnconstrained; /*�����û����¼*/*/
    Bitmask notIndexed;         /* Mask of tables that cannot use an index  mask�ı���Բ�ʹ������*/

    memset(&bestPlan, 0, sizeof(bestPlan));
    bestPlan.rCost = SQLITE_BIG_DBL;
=======
  for(i=iFrom=0, pLevel=pWInfo->a; i<nTabList; i++, pLevel++){	//ѭ������FROM�Ӿ��еı��б�
    WhereCost bestPlan;         /* Most efficient plan seen so far ��ĿǰΪֹ�ҵ�������Ч�ļƻ� */
    Index *pIdx;                /* Index for FROM table at pTabItem ��pTabItem��FROM�Ӿ��б�ʹ�õ����� */
    int j;                      /* For looping over FROM tables ѭ������FROM�Ӿ��еı� */
    int bestJ = -1;             /* The value of j j��ֵ */
    Bitmask m;                  /* Bitmask value for j or bestJ j��bestJ��λ�����ֵ */
    int isOptimal;              /* Iterator for optimal/non-optimal search ���/����������ĵ��� */
    int nUnconstrained;         /* Number tables without INDEXED BY û��INDEXED BY�ı���Ŀ */
    Bitmask notIndexed;         /* Mask of tables that cannot use an index ����ʹ��һ�������ı����� */

    memset(&bestPlan, 0, sizeof(bestPlan)); //ΪbestPlan�����ڴ�
    bestPlan.rCost = SQLITE_BIG_DBL; //��ʼ��ִ��bestPlan������ɱ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    WHERETRACE(("*** Begin search for loop %d ***\n", i));

    /* Loop through the remaining entries in the FROM clause to find the
    ** next nested loop. The loop tests all FROM clause entries
    ** either once or twice. 
	ͨ����FROM�Ӿ��е�ʣ����ѭ��Ѱ����һ��Ƕ��ѭ����
	ѭ���������е�FROM�Ӿ��е���һ�λ����Ρ�
    **
    ** ѭ��ͨ����FROM�Ӿ������������һ��Ƕ��ѭ����ѭ������һ�λ��������е�FROM�Ӿ��
    **
    ** The first test is always performed if there are two or more entries
    ** remaining and never performed if there is only one FROM clause entry
    ** to choose from.  The first test looks for an "optimal" scan.  In
    ** this context an optimal scan is one that uses the same strategy
    ** for the given FROM clause entry as would be selected if the entry
    ** were used as the innermost nested loop.  In other words, a table
    ** is chosen such that the cost of running that table cannot be reduced
    ** by waiting for other tables to run first.  This "optimal" test works
    ** by first assuming that the FROM clause is on the inner loop and finding
    ** its query plan, then checking to see if that query plan uses any
    ** other FROM clause terms that are notReady.  If no notReady terms are
    ** used then the "optimal" query plan works.
<<<<<<< HEAD
    **����ִ�е�һ���ԣ������ʣ���������������Ŀ��
	����δִ�У����������һ��FROM�Ӽ���Ŀѡ�񡣵�һ��
	����Ѱ��һ������ѡ���ɨ�衣����������µ���ѵ�ɨ��
	��һ��ʹ����ͬ�Ĳ������ڸ���FROM�Ӽ�������Ϊ�����
	��Ŀ���������ڲ�Ƕ��ѭ����ѡ�񡣻���֮��һ����ѡ
	��Ϊʹ�������иñ�ĳɱ��޷�ͨ���ȴ������������м��١�
	���ȼ���FROM�Ӿ������ڲ�ѭ�������ҵ��Լ��Ĳ�ѯ�ƻ���
	Ȼ���飬�����Ƿ��ܲ�ѯ�ƻ�ʹ���κ�����FROM�Ӽ���
	����δ�����������ѡ��Ĳ��Թ��������û��ʹ��δ�������
	Ȼ���ڡ���ѡ��Ĳ�ѯ�ƻ���������
=======
    **
    ** �����������������ʣ����ô����ִ�е�һ�β��ԣ����ֻ��һ��FROM�Ӿ���ɹ�ѡ����ô�ʹӲ�ִ�С�
    ** ��һ�β��Բ���һ��"��ѵ�"ɨ�衣����������������Ŀ���������ڲ�Ƕ��ѭ����
    ** ��ô����ɨ���Ϊ������FROM�Ӿ�ʹ����ͬ�Ĳ�����Ŀ��
    ** ���仰˵��ѡ��һ�����ñ���ͨ���ȴ����������������������д��ۡ�
    ** ���"��ѵ�"����ͨ����һ�μٶ�FROM�Ӿ������ڲ�ѭ���Ͳ������Ĳ�ѯ�ƻ�ʱ�����ã�
    ** ��ô����Ƿ��ѯ�Ż�ʹ�õ�����������FROM�Ӿ���δ׼���ġ�
    ** ���û��ʹ��δ׼������Ŀ����ô"��ѵ�"��ѯ�ƻ��������á�
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** Note that the WhereCost.nRow parameter for an optimal scan might
    ** not be as small as it would be if the table really were the innermost
    ** join.  The nRow value can be reduced by WHERE clause constraints
    ** that do not use indices.  But this nRow reduction only happens if the
    ** table really is the innermost join.  
	ע�⣬����һ�����ɨ��WhereCost.nRow�������ܲ�С����Ϊ���ǣ�
	��������������������ӡ���nRowֵ����ͨ��WHERE�Ӿ�Լ����ʹ
	��ָ�����١�����nRow����ֻ��������������������ļ��������¡�
    **
    ** ע��:����һ����Ѳ�ѯ��WhereCost.nRow�������ܲ�����������������ʱ����С��
    ** nRowֵ����ͨ����ʹ��������WHERE�Ӿ�Լ������С�����������С��nRowֵ�����ڱ����������������ӡ�
    **
    ** The second loop iteration is only performed if no optimal scan
    ** strategies were found by the first iteration. This second iteration
    ** is used to search for the lowest cost scan overall.
	��ִ�еڶ�ѭ����������ɵ�һ����û���ҵ���ѵ�ɨ����ԡ�
	��ڶ��ε��������������ɱ����ɨ�����塣

    **
    ** �����һ��û�з�������ɨ�������ô��ִ�еڶ���ѭ���������ڶ��ε������ڲ���ȫ��ɨ�����ʹ��ۡ�
    **
    ** Previous versions of SQLite performed only the second iteration -
    ** the next outermost loop was always that with the lowest overall
    ** cost. However, this meant that SQLite could select the wrong plan
    ** for scripts such as the following:
    **   ��ǰ�汾��SQLiteֻ�����˵ڶ��ε���-The�����������ѭ����������͵��ܳɱ������ǣ�����ζ��SQLite�Ŀ���ѡ���˴���ļƻ�
Ϊ�ű��������¼��㣺
    **   CREATE TABLE t1(a, b); ������t1��a,b��
    **   CREATE TABLE t2(c, d);������t2(c,d)
    **   SELECT * FROM t2, t1 WHERE t2.rowid = t1.a;
	��t1��t2�в�ѯ��������t2��rowid��ֵ����t1��aֵ
    **
    ** The best strategy is to iterate through table t1 first. However it
    ** is not possible to determine this with a simple greedy algorithm.
    ** Since the cost of a linear scan through table t2 is the same 
    ** as the cost of a linear scan through table t1, a simple greedy 
    ** algorithm may choose to use t2 for the outer loop, which is a much
    ** costlier approach.
<<<<<<< HEAD
	��õĲ�����ͨ����T1������һ��Ȼ���������޷�ȷ������һ���򵥵�̰���㷨��
	����ͨ����t2����ɨ��ĳɱ�����ͬ�ģ�ͨ����t1����ɨ��ĳɱ���
	�򵥵�̰���㷨����ѡ��ʹ��t2��������ѭ��������һ�ָ�����ķ�����
=======
    **
    ** SQLite��ǰ�İ汾ִֻ�еڶ��ε���--��һ��������ѭ�������ܳɱ���͵ġ�
    ** Ȼ��������ζ��SQLite����ѡ�����ļƻ������������������:
    **   CREATE TABLE t1(a, b); 
    **   CREATE TABLE t2(c, d);
    **   SELECT * FROM t2, t1 WHERE t2.rowid = t1.a;
    ** ��õĲ�������ѭ�����ʱ�t1��Ȼ����ʹ��һ���򵥵�̰���㷨�ǲ��ܾ�������ġ�
    ** ��������ɨ���t2�Ĵ��ۺ�����ɨ���t1�Ĵ�����ͬ������һ���򵥵�̰���㷨���ܰѱ�t2�����ⲿѭ����(���ֲ��Դ��۽ϸ�)
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    nUnconstrained = 0; //��ʼ��nUnconstrained
    notIndexed = 0; //��ʼ��notIndexed
    for(isOptimal=(iFrom<nTabList-1); isOptimal>=0 && bestJ<0; isOptimal--){ //ѭ��һ�λ��������������е�FROM�Ӿ���
      Bitmask mask;             /* Mask of tables not yet ready ��δ׼���ı����� */
      for(j=iFrom, pTabItem=&pTabList->a[j]; j<nTabList; j++, pTabItem++){ //ѭ������FROM�Ӿ��еı�
        int doNotReorder;    /* True if this table should not be reordered ������ܼ�¼�������ôΪTRUE */
        WhereCost sCost;     /* Cost information from best[Virtual]Index() best[Virtual]Index()�еĴ�����Ϣ */
        ExprList *pOrderBy;  /* ORDER BY clause for index to optimize �����Ż���ORDER BY�Ӿ� */
        ExprList *pDist;     /* DISTINCT clause for index to optimize �����Ż���DISTINCT�Ӿ� */
  
        doNotReorder =  (pTabItem->jointype & (JT_LEFT|JT_CROSS))!=0; //����������ӻ�CROSS���ӣ����¼�����
        if( j!=iFrom && doNotReorder ) break;
        m = getMask(pMaskSet, pTabItem->iCursor);
        if( (m & notReady)==0 ){
          if( j==iFrom ) iFrom++;
          continue;
        }
        mask = (isOptimal ? m : notReady);
        pOrderBy = ((i==0 && ppOrderBy )?*ppOrderBy:0);
        pDist = (i==0 ? pDistinct : 0);
        if( pTabItem->pIndex==0 ) nUnconstrained++;  //����û��ʹ��INDEXED BY�Ӿ�ı���Ŀ
  
        WHERETRACE(("=== trying table %d with isOptimal=%d ===\n",
                    j, isOptimal));
        assert( pTabItem->pTab ); //�ж�pTabItem->pTab�Ƿ�ΪNULL
#ifndef SQLITE_OMIT_VIRTUALTABLE
        if( IsVirtual(pTabItem->pTab) ){ //�жϱ��Ƿ�Ϊ���
          sqlite3_index_info **pp = &pWInfo->a[j].pIdxInfo; //��ʼ��**pp
          bestVirtualIndex(pParse, pWC, pTabItem, mask, notReady, pOrderBy,
                           &sCost, pp); //��������������
        }else 
#endif
        {
          bestBtreeIndex(pParse, pWC, pTabItem, mask, notReady, pOrderBy,
              pDist, &sCost); //��õ�Btree����
        }
        assert( isOptimal || (sCost.used&notReady)==0 );

        /* If an INDEXED BY clause is present, then the plan must use that
        ** index if it uses any index at all 
        **
        ** �������һ��INDEXED BY�Ӿ䣬��ô�ƻ�����ʹ��ORDER BY�Ӿ�ʹ�õ�������
        */
        assert( pTabItem->pIndex==0 
                  || (sCost.plan.wsFlags & WHERE_NOT_FULLSCAN)==0
                  || sCost.plan.u.pIdx==pTabItem->pIndex );

        if( isOptimal && (sCost.plan.wsFlags & WHERE_NOT_FULLSCAN)==0 ){
          notIndexed |= m;
        }

        /* Conditions under which this table becomes the best so far:
		������������Ϊ����Ϊֹ��õ�

        **
        **   (1) The table must not depend on other tables that have not
        **       yet run.�ñ���벻��������û��������
?�����С�
        **
        **   (2) A full-table-scan plan cannot supercede indexed plan unless
        **       the full-table-scan is an "optimal" plan as defined above.
		ȫ��ɨ��ƻ��޷�supercede�����ļƻ�������
ȫ��ɨ���ǡ����š��ļƻ��������ϡ�
	
        **
        **   (3) All tables have an INDEXED BY clause or this table lacks an
        **       INDEXED BY clause or this table uses the specific
        **       index specified by its INDEXED BY clause.  This rule ensures
        **       that a best-so-far is always selected even if an impossible
        **       combination of INDEXED BY clauses are given.  The error
        **       will be detected and relayed back to the application later.
        **       The NEVER() comes about because rule (2) above prevents
        **       An indexable full-table-scan from reaching rule (3).
        **���еı�����BY�Ӽ���ñ���û������BY�Ӿ��ñ�ʹ������¼�Ӿ�
		ָ�����ض��������˹���ȷ������ѵ���ôԶ����ѡ��ʹһ�������ܵ�
		�����¼���������������Ⲣ���ظ�Ӧ�ó�������
��NEVER������Լ����Ϊ����2��������ֹ��תλȫ��ɨ�赽�����3����
        **   (4) The plan cost must be lower than prior plans or else the
        **       cost must be the same and the number of rows must be lower.
<<<<<<< HEAD
		�üƻ��ĳɱ���������еļƻ��ǵͼ�����ɱ���������ͬ�ģ����е���Ŀ�����ǽϵ͵ġ�
=======
        **
        **
        **  ������˵�������ĿǰΪֹ����õĵ�����:
        **   (1) ����������������δ���еı�
        **   (2) һ��ȫ��ɨ��ƻ�����ȡ���������ļƻ�������ȫ��ɨ����һ�����涨���"��ѵ�"�ƻ�
        **   (3) ���еı���һ��INDEXED BY�Ӿ�������ȱ��һ��INDEXED BY�Ӿ�������ʹ�������ͨ������INDEXED BY�Ӿ�˵��������������
        **       ����涨ȷ������ѡ��һ��ĿǰΪֹ��õģ������Ǹ���һ�������ܵ�INDEXED BY�Ӿ���ϡ�
        **       ��鵽�������Ժ󽫴��ͻ�Ӧ�á�NEVER()��������Ϊ����Ĺ���(2)��ֹһ���ɼ�������ȫ��ɨ�����������(3)��
        **   (4) �ƻ��Ĵ��۱���С��ǰһ���ƻ����������ͬ���������Ƚ��١�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        */
        if( (sCost.used&notReady)==0                       /* (1) */
            && (bestJ<0 || (notIndexed&m)!=0               /* (2) */
                || (bestPlan.plan.wsFlags & WHERE_NOT_FULLSCAN)==0
                || (sCost.plan.wsFlags & WHERE_NOT_FULLSCAN)!=0)
            && (nUnconstrained==0 || pTabItem->pIndex==0   /* (3) */
                || NEVER((sCost.plan.wsFlags & WHERE_NOT_FULLSCAN)!=0))
            && (bestJ<0 || sCost.rCost<bestPlan.rCost      /* (4) */
                || (sCost.rCost<=bestPlan.rCost 
                 && sCost.plan.nRow<bestPlan.plan.nRow))
        ){
          WHERETRACE(("=== table %d is best so far"
                      " with cost=%g and nRow=%g\n",
                      j, sCost.rCost, sCost.plan.nRow));
          bestPlan = sCost; //����Ч�ļƻ��Ĵ���
          bestJ = j;
        }
        if( doNotReorder ) break;
      }
    }
    assert( bestJ>=0 );
    assert( notReady & getMask(pMaskSet, pTabList->a[bestJ].iCursor) );
    WHERETRACE(("*** Optimizer selects table %d for loop %d"
                " with cost=%g and nRow=%g\n",
                bestJ, pLevel-pWInfo->a, bestPlan.rCost, bestPlan.plan.nRow));
    /* The ALWAYS() that follows was added to hush up clang scan-build �������ALWAYS()�����ڸ�clang scan-build */
    if( (bestPlan.plan.wsFlags & WHERE_ORDERBY)!=0 && ALWAYS(ppOrderBy) ){
      *ppOrderBy = 0;
    }
    if( (bestPlan.plan.wsFlags & WHERE_DISTINCT)!=0 ){
      assert( pWInfo->eDistinct==0 );
      pWInfo->eDistinct = WHERE_DISTINCT_ORDERED;
    }
    andFlags &= bestPlan.plan.wsFlags;
    pLevel->plan = bestPlan.plan;
    testcase( bestPlan.plan.wsFlags & WHERE_INDEXED );
    testcase( bestPlan.plan.wsFlags & WHERE_TEMP_INDEX );
    if( bestPlan.plan.wsFlags & (WHERE_INDEXED|WHERE_TEMP_INDEX) ){
      if( (wctrlFlags & WHERE_ONETABLE_ONLY) 
       && (bestPlan.plan.wsFlags & WHERE_TEMP_INDEX)==0 
      ){
        pLevel->iIdxCur = iIdxCur;
      }else{
        pLevel->iIdxCur = pParse->nTab++;
      }
    }else{
      pLevel->iIdxCur = -1;
    }
    notReady &= ~getMask(pMaskSet, pTabList->a[bestJ].iCursor);
    pLevel->iFrom = (u8)bestJ;
    if( bestPlan.plan.nRow>=(double)1 ){
      pParse->nQueryLoop *= bestPlan.plan.nRow;
    }

    /* Check that if the table scanned by this loop iteration had an
    ** INDEXED BY clause attached to it, that the named index is being
    ** used for the scan. If not, then query compilation has failed.
    ** Return an error.
<<<<<<< HEAD
	��飬���ͨ����ѭ������ɨ��ı�������BY�Ӿ����ӵ�����
	������������������ɨ�衣���û�У���ô�ڲ�ѯ����ʧ�ܡ�
?����һ������
=======
    **
    ** ��ͨ�����ѭ������ɨ��ʱ��������Ƿ���һ��INDEXED BY�Ӿ䣬����У���ô��ʹ�����ָ��������������ɨ�衣
    ** ���û�У���ô��ѯ�༭��ʧ�ܡ�����һ������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    pIdx = pTabList->a[bestJ].pIndex;
    if( pIdx ){
      if( (bestPlan.plan.wsFlags & WHERE_INDEXED)==0 ){
        sqlite3ErrorMsg(pParse, "cannot use index: %s", pIdx->zName);
        goto whereBeginError;
      }else{
        /* If an INDEXED BY clause is used, the bestIndex() function is
        ** guaranteed to find the index specified in the INDEXED BY clause
        ** if it find an index at all. 
<<<<<<< HEAD
		���һ������BY�Ӽ�����bestIndex����������֤�ҵ�ָ��������
		��¼��������ҵ�һ�������ġ�
		*/
=======
        **
        ** ���ʹ��һ��INDEXED BY�Ӿ䣬���bestIndex()�������ҵ�һ��������
        ** ��ô��֤�����ҵ���INDEXED BY�Ӿ��е�ָ��������
        */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
        assert( bestPlan.plan.u.pIdx==pIdx );
      }
    }
  }
  WHERETRACE(("*** Optimizer Finished ***\n"));
  if( pParse->nErr || db->mallocFailed ){
    goto whereBeginError;
  }

  /* If the total query only selects a single row, then the ORDER BY
  ** clause is irrelevant.
<<<<<<< HEAD
  ����ܲ�ѯ��ѡ��һ���У�Ȼ����ORDER BY
�Ӽ��ǲ���صġ�
=======
  **
  ** �����ѯֻ��ѡ��һ�У���ôORDER BY�Ӿ�����޹�ʹ���ġ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  if( (andFlags & WHERE_UNIQUE)!=0 && ppOrderBy ){
    *ppOrderBy = 0;
  }

  /* If the caller is an UPDATE or DELETE statement that is requesting
  ** to use a one-pass algorithm, determine if this is appropriate.
  ** The one-pass algorithm only works if the WHERE clause constraints
  ** the statement to update a single row.
<<<<<<< HEAD
  �����������һ��UPDATE��DELETE�������ʹ��һ��ͨ�㷨��
  ȷ�����Ƿ���������һ���ϸ���㷨ֻ�����WHERE�Ӿ����Ƶ�������һ�С�
=======
  **
  ** ������÷�ʽһ��UPDATE��DELETE��������һ��һ��ͨ�����㷨��ȷ�����Ǻ��ʵġ�
  ** һ��ͨ���㷨ֵ��WHERE���Ӿ�Լ������ȥ����һ��ʱ�������á�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  assert( (wctrlFlags & WHERE_ONEPASS_DESIRED)==0 || pWInfo->nLevel==1 );
  if( (wctrlFlags & WHERE_ONEPASS_DESIRED)!=0 && (andFlags & WHERE_UNIQUE)!=0 ){
    pWInfo->okOnePass = 1;
    pWInfo->a[0].plan.wsFlags &= ~WHERE_IDX_ONLY;
  }

  /* Open all tables in the pTabList and any indices selected for
  ** searching those tables.
<<<<<<< HEAD
  ����ѡ������Щ���е�pTabList���б���������κ�
=======
  **
  ** ��pTabList�����еı������������Щ�����ѡ��������������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  sqlite3CodeVerifySchema(pParse, -1); /* Insert the cookie verifier Goto ��֤��Goto����cookie */
  notReady = ~(Bitmask)0;
  pWInfo->nRowOut = (double)1;
  for(i=0, pLevel=pWInfo->a; i<nTabList; i++, pLevel++){
<<<<<<< HEAD
    Table *pTab;     /* Table to open �򿪱�*/
    int iDb;         /* Index of database containing table/index  ���ݿ�����ı�/����������*/
=======
    Table *pTab;     /* Table to open ��Ҫ�򿪵ı� */
    int iDb;         /* Index of database containing table/index ���ݿ�����������/���� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949

    pTabItem = &pTabList->a[pLevel->iFrom];
    pTab = pTabItem->pTab;
    pLevel->iTabCur = pTabItem->iCursor;
    pWInfo->nRowOut *= pLevel->plan.nRow;
    iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
    if( (pTab->tabFlags & TF_Ephemeral)!=0 || pTab->pSelect ){
      /* Do nothing ʲô������ */
    }else
#ifndef SQLITE_OMIT_VIRTUALTABLE
    if( (pLevel->plan.wsFlags & WHERE_VIRTUALTABLE)!=0 ){
      const char *pVTab = (const char *)sqlite3GetVTable(db, pTab);
      int iCur = pTabItem->iCursor;
      sqlite3VdbeAddOp4(v, OP_VOpen, iCur, 0, 0, pVTab, P4_VTAB);
    }else
#endif
    if( (pLevel->plan.wsFlags & WHERE_IDX_ONLY)==0
         && (wctrlFlags & WHERE_OMIT_OPEN_CLOSE)==0 ){
      int op = pWInfo->okOnePass ? OP_OpenWrite : OP_OpenRead;
      sqlite3OpenTable(pParse, pTabItem->iCursor, iDb, pTab, op);
      testcase( pTab->nCol==BMS-1 );
      testcase( pTab->nCol==BMS );
      if( !pWInfo->okOnePass && pTab->nCol<BMS ){
        Bitmask b = pTabItem->colUsed;
        int n = 0;
        for(; b; b=b>>1, n++){}
        sqlite3VdbeChangeP4(v, sqlite3VdbeCurrentAddr(v)-1, 
                            SQLITE_INT_TO_PTR(n), P4_INT32);
        assert( n<=pTab->nCol );
      }
    }else{
      sqlite3TableLock(pParse, iDb, pTab->tnum, 0, pTab->zName);
    }
#ifndef SQLITE_OMIT_AUTOMATIC_INDEX
    if( (pLevel->plan.wsFlags & WHERE_TEMP_INDEX)!=0 ){
      constructAutomaticIndex(pParse, pWC, pTabItem, notReady, pLevel);
    }else
#endif
    if( (pLevel->plan.wsFlags & WHERE_INDEXED)!=0 ){
      Index *pIx = pLevel->plan.u.pIdx;
      KeyInfo *pKey = sqlite3IndexKeyinfo(pParse, pIx);
      int iIndexCur = pLevel->iIdxCur;
      assert( pIx->pSchema==pTab->pSchema );
      assert( iIndexCur>=0 );
      sqlite3VdbeAddOp4(v, OP_OpenRead, iIndexCur, pIx->tnum, iDb,
                        (char*)pKey, P4_KEYINFO_HANDOFF);
      VdbeComment((v, "%s", pIx->zName));
    }
    sqlite3CodeVerifySchema(pParse, iDb);
    notReady &= ~getMask(pWC->pMaskSet, pTabItem->iCursor);
  }
  pWInfo->iTop = sqlite3VdbeCurrentAddr(v);
  if( db->mallocFailed ) goto whereBeginError;

  /* Generate the code to do the search.  Each iteration of the for
  ** loop below generates code for a single nested loop of the VM
  ** program.
  **
  ** ���ɴ��������������������forѭ��ÿ�ε���ΪVM�����һ��Ƕ��ѭ�����ɴ��롣
  */
  notReady = ~(Bitmask)0;
  for(i=0; i<nTabList; i++){
    pLevel = &pWInfo->a[i];
    explainOneScan(pParse, pTabList, pLevel, i, pLevel->iFrom, wctrlFlags);
    notReady = codeOneLoopStart(pWInfo, i, wctrlFlags, notReady);
    pWInfo->iContinue = pLevel->addrCont;
  }

#ifdef SQLITE_TEST  /* For testing and debugging use only ֻ���ڲ��Ժ͵��� */
  /* Record in the query plan information about the current table
  ** and the index used to access it (if any).  If the table itself
  ** is not used, its name is just '{}'.  If no index is used
  ** the index is listed as "{}".  If the primary key is used the
  ** index name is '*'.
<<<<<<< HEAD
  ���ڲ��Ժ͵���ֻ�ü�¼���йص�ǰ��Ĳ�ѯ�ƻ���Ϣ
�����õ�������������������еĻ������������ʹ��ʱ��
��������ֻ�ǡ�{}�������û��ʹ����������������Ϊ��{}����
�������ʹ�õ�������������'*'��
=======
  **
  ** ��¼�ڲ�ѯ�ƻ����йص�ǰ����������ʵ���Ϣ�����δʹ�ñ�����ô��������ֻ��'{}'��
  ** ���û��������ʹ�ã���������Ϊ"{}".���ʹ������������ô��������'*'
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  for(i=0; i<nTabList; i++){
    char *z;
    int n;
    pLevel = &pWInfo->a[i];
    pTabItem = &pTabList->a[pLevel->iFrom];
    z = pTabItem->zAlias;
    if( z==0 ) z = pTabItem->pTab->zName;
    n = sqlite3Strlen30(z);
    if( n+nQPlan < sizeof(sqlite3_query_plan)-10 ){
      if( pLevel->plan.wsFlags & WHERE_IDX_ONLY ){
        memcpy(&sqlite3_query_plan[nQPlan], "{}", 2);
        nQPlan += 2;
      }else{
        memcpy(&sqlite3_query_plan[nQPlan], z, n);
        nQPlan += n;
      }
      sqlite3_query_plan[nQPlan++] = ' ';
    }
    testcase( pLevel->plan.wsFlags & WHERE_ROWID_EQ );
    testcase( pLevel->plan.wsFlags & WHERE_ROWID_RANGE );
    if( pLevel->plan.wsFlags & (WHERE_ROWID_EQ|WHERE_ROWID_RANGE) ){
      memcpy(&sqlite3_query_plan[nQPlan], "* ", 2);
      nQPlan += 2;
    }else if( (pLevel->plan.wsFlags & WHERE_INDEXED)!=0 ){
      n = sqlite3Strlen30(pLevel->plan.u.pIdx->zName);
      if( n+nQPlan < sizeof(sqlite3_query_plan)-2 ){
        memcpy(&sqlite3_query_plan[nQPlan], pLevel->plan.u.pIdx->zName, n);
        nQPlan += n;
        sqlite3_query_plan[nQPlan++] = ' ';
      }
    }else{
      memcpy(&sqlite3_query_plan[nQPlan], "{} ", 3);
      nQPlan += 3;
    }
  }
  while( nQPlan>0 && sqlite3_query_plan[nQPlan-1]==' ' ){
    sqlite3_query_plan[--nQPlan] = 0;
  }
  sqlite3_query_plan[nQPlan] = 0;
  nQPlan = 0;
#endif 
  /* SQLITE_TEST // Testing and debugging use only ֻ���ڲ��Ժ͵��� */

  /* Record the continuation address in the WhereInfo structure.  Then
  ** clean up and return.
<<<<<<< HEAD
  ��¼��WhereInfo�ṹ�����ĵ�ַ��Ȼ�����������ء�
  */
  return pWInfo;

  /* Jump here if malloc fails  ���mallocʧ�ܣ���ת������*/
=======
  **
  ** ��¼��WhereInfo���ݽṹ�е�������ַ��Ȼ����������ء�
  */
  return pWInfo;

  /* Jump here if malloc fails ��������ڴ�ʧ�ܾ����� */
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
whereBeginError:
  if( pWInfo ){
    pParse->nQueryLoop = pWInfo->savedNQueryLoop;
    whereInfoFree(db, pWInfo);
  }
  return 0;
}

/*
** Generate the end of the WHERE loop.  See comments on 
** sqlite3WhereBegin() for additional information.
<<<<<<< HEAD
������WHEREѭ���Ľ������鿴sqlite3WhereBegin���ۣ����˽������Ϣ��
=======
**
** ����WHEREѭ���Ľ������롣�鿴��sqlite3WhereBegin()�ϵĸ�����Ϣ�����ۡ�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
*/
void sqlite3WhereEnd(WhereInfo *pWInfo){
  Parse *pParse = pWInfo->pParse;
  Vdbe *v = pParse->pVdbe;
  int i;
  WhereLevel *pLevel;
  SrcList *pTabList = pWInfo->pTabList;
  sqlite3 *db = pParse->db;

  /* Generate loop termination code.
<<<<<<< HEAD
  ����ѭ����ֹ����
=======
  ** ����ѭ����ֹ����
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  sqlite3ExprCacheClear(pParse);
  for(i=pWInfo->nLevel-1; i>=0; i--){
    pLevel = &pWInfo->a[i];
    sqlite3VdbeResolveLabel(v, pLevel->addrCont);
    if( pLevel->op!=OP_Noop ){
      sqlite3VdbeAddOp2(v, pLevel->op, pLevel->p1, pLevel->p2);
      sqlite3VdbeChangeP5(v, pLevel->p5);
    }
    if( pLevel->plan.wsFlags & WHERE_IN_ABLE && pLevel->u.in.nIn>0 ){
      struct InLoop *pIn;
      int j;
      sqlite3VdbeResolveLabel(v, pLevel->addrNxt);
      for(j=pLevel->u.in.nIn, pIn=&pLevel->u.in.aInLoop[j-1]; j>0; j--, pIn--){
        sqlite3VdbeJumpHere(v, pIn->addrInTop+1);
        sqlite3VdbeAddOp2(v, OP_Next, pIn->iCur, pIn->addrInTop);
        sqlite3VdbeJumpHere(v, pIn->addrInTop-1);
      }
      sqlite3DbFree(db, pLevel->u.in.aInLoop);
    }
    sqlite3VdbeResolveLabel(v, pLevel->addrBrk);
    if( pLevel->iLeftJoin ){
      int addr;
      addr = sqlite3VdbeAddOp1(v, OP_IfPos, pLevel->iLeftJoin);
      assert( (pLevel->plan.wsFlags & WHERE_IDX_ONLY)==0
           || (pLevel->plan.wsFlags & WHERE_INDEXED)!=0 );
      if( (pLevel->plan.wsFlags & WHERE_IDX_ONLY)==0 ){
        sqlite3VdbeAddOp1(v, OP_NullRow, pTabList->a[i].iCursor);
      }
      if( pLevel->iIdxCur>=0 ){
        sqlite3VdbeAddOp1(v, OP_NullRow, pLevel->iIdxCur);
      }
      if( pLevel->op==OP_Return ){
        sqlite3VdbeAddOp2(v, OP_Gosub, pLevel->p1, pLevel->addrFirst);
      }else{
        sqlite3VdbeAddOp2(v, OP_Goto, 0, pLevel->addrFirst);
      }
      sqlite3VdbeJumpHere(v, addr);
    }
  }

  /* The "break" point is here, just past the end of the outer loop.
  ** Set it.
<<<<<<< HEAD
  �ڡ�break����һ����������ոչ�ȥ����ѭ���Ľ�����
��������
=======
  ** "break"ָ�롣�ոս�����ѭ������������
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  sqlite3VdbeResolveLabel(v, pWInfo->iBreak);

  /* Close all of the cursors that were opened by sqlite3WhereBegin.
<<<<<<< HEAD
  �ر�����sqlite3WhereBegin���򿪵��αꡣ
=======
  ** �ر�������sqlite3WhereBegin�򿪵��α�
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
  */
  assert( pWInfo->nLevel==1 || pWInfo->nLevel==pTabList->nSrc );
  for(i=0, pLevel=pWInfo->a; i<pWInfo->nLevel; i++, pLevel++){
    Index *pIdx = 0;
    struct SrcList_item *pTabItem = &pTabList->a[pLevel->iFrom];
    Table *pTab = pTabItem->pTab;
    assert( pTab!=0 );
    if( (pTab->tabFlags & TF_Ephemeral)==0
     && pTab->pSelect==0
     && (pWInfo->wctrlFlags & WHERE_OMIT_OPEN_CLOSE)==0
    ){
      int ws = pLevel->plan.wsFlags;
      if( !pWInfo->okOnePass && (ws & WHERE_IDX_ONLY)==0 ){
        sqlite3VdbeAddOp1(v, OP_Close, pTabItem->iCursor);
      }
      if( (ws & WHERE_INDEXED)!=0 && (ws & WHERE_TEMP_INDEX)==0 ){
        sqlite3VdbeAddOp1(v, OP_Close, pLevel->iIdxCur);
      }
    }

    /* If this scan uses an index, make code substitutions to read data
    ** from the index in preference to the table. Sometimes, this means
    ** the table need never be read from. This is a performance boost,
    ** as the vdbe level waits until the table is read before actually
    ** seeking the table cursor to the record corresponding to the current
    ** position in the index.
<<<<<<< HEAD
    ** �����ɨ��ʹ��������ʹ�����滻�Դӱ��е��������ȶ�ȡ���ݡ�
	��ʱ������ζ�Ÿñ���Ҫ��Զ���ᱻ�Ӷ���������һ������������
	��ΪVDBEˮƽ�ȴ�ֱ����ʵ�ʽ����������Ӧ�������ĵ�ǰλ�ü�¼֮ǰ������
=======
    ** 
    ** ������ɨ��ʹ����һ����������д����ӱ��ж�ȡ����ȡ�����ȴ������ж�ȡ���ݡ�
    ** ��ʱ������ζ�ű����ڲ���Ҫ����ȡ��
    ** ����һ�������ƽ�����Ѱ�Ҷ�Ӧ�ı���α�����¼��ǰ������λ��֮ǰ��vdbeˮƽһֱ�ȴ���ȡ��
    **
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    ** Calls to the code generator in between sqlite3WhereBegin and
    ** sqlite3WhereEnd will have created code that references the table
    ** directly.  This loop scans all that code looking for opcodes
    ** that reference the table and converts them into opcodes that
    ** reference the index.
<<<<<<< HEAD
	���ô�����������sqlite3WhereBegin��sqlite3WhereEnd֮�佫��ֱ�����õı����Ĵ��롣
	���ѭ��ɨ�����еĴ���Ѱ�Ҳ��������ñ�������ת�����������������롣
=======
    **
    ** ��sqlite3WhereBegin��sqlite3WhereEnd֮����ô���������ֱ�ӵش��������صĴ��롣
    ** ���ѭ��ɨ��������Щ������Ѱ�������صĲ����벢������ת��Ϊ��������صĲ����롣
>>>>>>> 91288352e83e9763d493ed84aec377d15ced3949
    */
    if( pLevel->plan.wsFlags & WHERE_INDEXED ){
      pIdx = pLevel->plan.u.pIdx;
    }else if( pLevel->plan.wsFlags & WHERE_MULTI_OR ){
      pIdx = pLevel->u.pCovidx;
    }
    if( pIdx && !db->mallocFailed){
      int k, j, last;
      VdbeOp *pOp;

      pOp = sqlite3VdbeGetOp(v, pWInfo->iTop);
      last = sqlite3VdbeCurrentAddr(v);
      for(k=pWInfo->iTop; k<last; k++, pOp++){
        if( pOp->p1!=pLevel->iTabCur ) continue;
        if( pOp->opcode==OP_Column ){
          for(j=0; j<pIdx->nColumn; j++){
            if( pOp->p2==pIdx->aiColumn[j] ){
              pOp->p2 = j;
              pOp->p1 = pLevel->iIdxCur;
              break;
            }
          }
          assert( (pLevel->plan.wsFlags & WHERE_IDX_ONLY)==0
               || j<pIdx->nColumn );
        }else if( pOp->opcode==OP_Rowid ){
          pOp->p1 = pLevel->iIdxCur;
          pOp->opcode = OP_IdxRowid;
        }
      }
    }
  }
  /* Final cleanup 
  ** ����������
  */
  pParse->nQueryLoop = pWInfo->savedNQueryLoop;
  whereInfoFree(db, pWInfo);
  return;
}
