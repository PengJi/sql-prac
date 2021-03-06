#include <mpi.h>

#include "postgres.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "utils/fmgroids.h"
#include "utils/tqual.h"
#include "utils/builtins.h"
#include "executor/spi.h"

#define MAX_N 4096
#define PI 3.1415926535897932
#define EPS 10E-8
#define V_TAG 99
#define P_TAG 100
#define Q_TAG 101
#define R_TAG 102
#define S_TAG 103
#define S_TAG2 104
#define MAX_LINE 16384

PG_MODULE_MAGIC;

//typedef enum {FALSE,TRUE} BOOL;
typedef int BOOL; 

typedef struct 
{
    double r;
    double i;
}complex_t;

complex_t p[MAX_N],s[2*MAX_N],r[2*MAX_N];
complex_t w[2*MAX_N];
uint64 variableNum;
double transTime=0,totalTime=0,beginTime;
MPI_Status status;

/**
 * [comp_add description]
 * @param result [description]
 * @param c1     [description]
 * @param c2     [description]
 */
void comp_add(complex_t* result,const complex_t* c1,const complex_t* c2)
{
    result->r=c1->r+c2->r;
    result->i=c1->i+c2->i;
}

/**
 * [comp_multiply description]
 * @param result [description]
 * @param c1     [description]
 * @param c2     [description]
 */
void comp_multiply(complex_t* result,const complex_t* c1,const complex_t* c2)
{
    result->r=c1->r*c2->r-c1->i*c2->i;
    result->i=c1->r*c2->i+c2->r*c1->i;
}

/**
 * @brief      移动f中从beginPos到endPos位置的元素，使之按位置奇偶
 * 				重新排列。举例说明:假设数组f，beginPos=2, endPos=5
 * 				则shuffle函数的运行结果为f[2..5]重新排列，排列后各个
 * 				位置对应的原f的元素为: f[2],f[4],f[3],f[5]
 *
 * @param      f         被操作数组首地址
 * @param[in]  beginPos  数据的开始位置
 * @param[in]  endPos    数组的结束位置
 */
void shuffle(complex_t* f, int beginPos, int endPos)
{
    int i;
    complex_t temp[2*MAX_N];

    for(i = beginPos; i <= endPos; i ++)
    {
        temp[i] = f[i];
    }

    int j = beginPos;
    for(i = beginPos; i <= endPos; i +=2)
    {
        f[j] = temp[i];
        j++;
    }
    for(i = beginPos +1; i <= endPos; i += 2)
    {
        f[j] = temp[i];
        j++;
    }
}

/**
 * @brief      对复数序列f进行FFT或者IFFT(由x决定)，结果序列为y，
 * 				产生leftPos 到 rightPos之间的结果元素
 *
 * @param      f            原始序列数组首地址
 * @param[in]  beginPos     原始序列在数组f中的第一个下标
 * @param[in]  endPos       原始序列在数组f中的最后一个下标
 * @param[in]  x            存放单位根的数组(可视为旋转因子)，其元素为w,w^2,w^3...
 * @param      y            输出序列
 * @param[in]  leftPos      所负责计算输出的y的片断的起始下标
 * @param[in]  rightPos     所负责计算输出的y的片断的终止下标
 * @param[in]  totalLength  y的长度
 */
void evaluate(complex_t* f, int beginPos, int endPos,const complex_t* x, complex_t* y,
int leftPos, int rightPos, int totalLength)
{
    int i;
    if ((beginPos > endPos)||(leftPos > rightPos))
    {
        printf("Error in use Polynomial!\n");
        exit(-1);
    }
    else if(beginPos == endPos)
    {
        for(i = leftPos; i <= rightPos; i ++)
        {
            y[i] = f[beginPos];
        }
    }
    else if(beginPos + 1 == endPos)
    {
        for(i = leftPos; i <= rightPos; i ++)
        {
            complex_t temp;
            comp_multiply(&temp, &f[endPos], &x[i]);
            comp_add(&y[i], &f[beginPos], &temp);
        }
    }
    else
    {
        complex_t tempX[2*MAX_N],tempY1[2*MAX_N], tempY2[2*MAX_N];
        int midPos = (beginPos + endPos)/2;

        shuffle(f, beginPos, endPos);

        for(i = leftPos; i <= rightPos; i ++)
        {
            comp_multiply(&tempX[i], &x[i], &x[i]);
        }

        evaluate(f, beginPos, midPos, tempX, tempY1,
            leftPos, rightPos, totalLength);
        evaluate(f, midPos+1, endPos, tempX, tempY2,
            leftPos, rightPos, totalLength);

        for(i = leftPos; i <= rightPos; i ++)
        {
            complex_t temp;
            comp_multiply(&temp, &x[i], &tempY2[i]);
            comp_add(&y[i], &tempY1[i], &temp);
        }
    }
}

/*
 * Function:    print
 * Description: 打印数组元素的实部
 * Parameters:  f为待打印数组的首地址
 *              fLength为数组的长度
 */
void print_ereport(const complex_t* f, int fLength)
{
    BOOL isPrint = FALSE;
    int i;

    // f[0]
    if (abs(f[0].r) > EPS)
    {
        ereport(INFO,(errmsg("%f", f[0].r)));
        isPrint = TRUE;
    }

    for(i = 1; i < fLength; i ++)
    {
        if (f[i].r > EPS)
        {
            if (isPrint)
            	ereport(INFO,(errmsg(" + ")));
            else
                isPrint = TRUE;
            ereport(INFO,(errmsg("%ft^%d", f[i].r, i)));
        }
        else if (f[i].r < - EPS)
        {
            if(isPrint)
            	ereport(INFO,(errmsg(" - ")));
            else
                isPrint = TRUE;
            ereport(INFO,(errmsg("%ft^%d", -f[i].r, i)));
        }
    }
    if (isPrint == FALSE)
    	ereport(INFO,(errmsg("0")));
}

/*
 * Function:    myprint
 * Description: 完整打印复数数组元素，包括实部和虚部
 * Parameters:  f为待打印数组的首地址
 *              fLength为数组的长度
 */
void myprint_ereport(const complex_t* f, int fLength)
{
    int i;
    for(i=0;i<fLength;i++)
    {
        ereport(INFO,(errmsg("%f+%fi , ", f[i].r, f[i].i)));
    }
}

/*
 * 打印结果
 */
void printres_ereport(const complex_t* f,int fLength)
{
	int i;

	for(i=0;i<fLength;i+=2)
	{		
		if(f[i].i<0)
			ereport(INFO,(errmsg("%f-%fi\n",f[i].r,-f[i].i)));
		else
			ereport(INFO,(errmsg("%f+%fi\n",f[i].r,f[i].i)));
	}
}

/*
 * 添加运行时间
 */
void addTransTime(double toAdd)
{
	transTime+=toAdd;
}

/*
 * Function:    sendOrigData
 * Description: 把原始数据发送给其它进程
 * Parameters:  size为集群中进程的数目
 */
void sendOrigData(int size)
{
	int i;

	for(i=1;i<size;i++)
	{
		//向所有进程发送数据的总个数
		MPI_Send(&variableNum,1,MPI_INT,i,V_TAG,MPI_COMM_WORLD);
		//向所有进程发送数据
		MPI_Send(p, variableNum * 2, MPI_DOUBLE, i, P_TAG, MPI_COMM_WORLD);
	}

}

/*
 * Function:    recvOrigData
 * Description:	接受原始数据，从进程0接收消息
 */
void recvOrigData()
{
	//从进程0接收数据的总个数
	MPI_Recv(&variableNum,1,MPI_INT,0,V_TAG,MPI_COMM_WORLD,&status);
	//从进程0接收所有的数据
	MPI_Recv(p, variableNum * 2, MPI_DOUBLE, 0, P_TAG, MPI_COMM_WORLD, &status);
}

/*
 * fft的udf实现
 */
PG_FUNCTION_INFO_V1(fft_main);
Datum
fft_main(PG_FUNCTION_ARGS)
{
	int rank,size,i;
	int32 arg = PG_GETARG_INT32(0);

	MPI_Init(NULL,NULL);
	MPI_Comm_rank(MPI_COMM_WORLD,&rank);
	MPI_Comm_size(MPI_COMM_WORLD,&size);

	// 初始进程
	if(rank==0)
	{
		// 0# 进程从文件读入多项式p的阶数和系数序列
		// if(!readFromDB(MAX_LINE))
		// 	exit(-1);
		
		/*
		//从数据库表中读取数据
		char *command="select val from test order by id";
	    int ret;
	    uint64 proc;
	    float r;

	    //command = text_to_cstring(PG_GETARG_TEXT_P(0));

	    SPI_connect();
	    ret = SPI_exec(command, MAX_LINE);
	    variableNum=SPI_processed;
	    proc = SPI_processed;

		if((variableNum<1)||(variableNum>MAX_N))
		{
			ereport(INFO,(errmsg("variableNum out of range!")));
			return(FALSE);
		}
		ereport(INFO,(errmsg("variableNum=%d",variableNum)));

	    if (ret > 0 && SPI_tuptable != NULL){
	        TupleDesc tupdesc = SPI_tuptable->tupdesc;
	        SPITupleTable *tuptable = SPI_tuptable;
	        char buf[10];
	        uint64 j;

	        for (j = 0; j < proc; j++) //proc为表的行数
	        {
	            HeapTuple tuple = tuptable->vals[j];

	            for (i = 1, buf[0] = 0; i <= tupdesc->natts; i++){
	                snprintf(buf + strlen (buf), sizeof(buf) - strlen(buf), " %s%s",
	                        SPI_getvalue(tuple, tupdesc, i),
	                        (i == tupdesc->natts) ? " " : " |");
	            }

	            ereport(INFO,(errmsg("ROW: %s",buf))); //输出一行数据
	            sscanf(buf,"%f",&r);
				//准备数据
	            p[j].r = r;
	            p[j].i = 0.0f;
	        }
	    }

		// 打印原始数组
		// ereport(INFO,(errmsg("p(t)=")));
		// print_ereport(p,variableNum);

		SPI_finish();
		*/

		//测试数据
		variableNum=4;
		p[0].r = 1.0; p[0].i = 0.0;
		p[1].r = 2.0; p[1].i = 0.0;
		p[2].r = 4.0; p[2].i = 0.0;
		p[3].r = 3.0; p[3].i = 0.0;

		// 进程数目太多，造成每个进程平均分配不到一个元素，异常退出
		if(size>2*variableNum)
		{
			ereport(INFO,(errmsg("Too many Processors,reduce your -np value")));
			MPI_Abort(MPI_COMM_WORLD,1);
		}

		beginTime=MPI_Wtime();

		// 0#进程把多项式的阶数,p发送给其它进程
		sendOrigData(size);

		// 累计传输时间
		addTransTime(MPI_Wtime()-beginTime);

	}else{ // 其它进程接收进程0发送来的数据，包括variableNum、数组p
		recvOrigData();
	}

	// 初始化数组w，用于进行傅立叶变换
	int wLength=2*variableNum;
	for(i=0;i<wLength;i++)
	{
		w[i].r=cos(i*2*PI/wLength);
		w[i].i=sin(i*2*PI/wLength);
	}

	// 划分各个进程的工作范围 startPos ~ stopPos
	int everageLength=wLength/size; // 8/4=2(假设有四个进程)
	int moreLength=wLength%size; // 8%4=0
	int startPos=moreLength+rank*everageLength; // 0+0*2=0; 0+1*2=1; 0+2*2=4; 0+3*2=6
	int stopPos=startPos+everageLength-1; // 0+2-1=1; 1+2-1=2; 4+2-1=5; 6+2-1=7
	//片段: [0,1], [1,2], [4,5], [6,7]

	if(rank==0)
	{
		startPos=0;
		stopPos=moreLength+everageLength-1;
	}

    // 对p作FFT，输出序列为s，每个进程仅负责计算出序列中位置为startPos 到 stopPos的元素
	evaluate(p,0,variableNum-1,w,s,startPos,stopPos,wLength);
	
	ereport(INFO,(errmsg("partial results, process %d.",rank)));
	myprint_ereport(s,wLength);
	
	// 各个进程都把s中自己负责计算出来的部分发送给进程0，并从进程0接收汇总的s
	if(rank>0)
	{
		MPI_Send(s+startPos,everageLength*2,MPI_DOUBLE,0,S_TAG,MPI_COMM_WORLD);
		MPI_Recv(s,wLength*2,MPI_DOUBLE,0,S_TAG2,MPI_COMM_WORLD,&status);
	}
	else // 进程0接收s片段，向其余进程发送完整的s
	{
		double tempTime=MPI_Wtime();

		// 进程0接收s片段
		for(i=1;i<size;i++)
		{
			MPI_Recv(s+moreLength+i*everageLength,everageLength*2,MPI_DOUBLE,i,S_TAG,MPI_COMM_WORLD,&status);
		}

		//向其余进程发送完整的结果s
		for(i=1;i<size;i++)
		{
			MPI_Send(s,wLength*2,MPI_DOUBLE,i,S_TAG2,MPI_COMM_WORLD);
		}

		ereport(INFO,(errmsg("The final results :")));
		printres_ereport(s,wLength);

		addTransTime(MPI_Wtime()-tempTime);
	}

	if(rank==0)
	{
		totalTime=MPI_Wtime();
		totalTime-=beginTime;

		ereport(INFO,(errmsg("Use prossor size=%d",size)));
		ereport(INFO,(errmsg("Total running time=%f(s)",totalTime)));
		ereport(INFO,(errmsg("Distribute data time = %f(s)",transTime)));
		ereport(INFO,(errmsg("Parallel compute time = %f(s) ",totalTime-transTime)));
	}

	MPI_Finalize();

	PG_RETURN_INT32(arg);
}
