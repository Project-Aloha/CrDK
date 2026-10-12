/* Actual shared transport replay: wire remote intent IDs are uint32, including0.
 * Linux qcom_glink_handle_intent stores the exact ID in riids without a lower
 * bound of1; locally advertised receive IDs separately start at1. MIT */
#define main ExistingTypeCWireMain
#include "UsbTypeCTransportTest.c"
#undef main
int main(void)
{
 UINT8 Data[12]={0};UINT32 Pair[2]={1024,0};
 Fixture();Packet(5,77,1,Pair,sizeof(Pair));
 assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS); /* existing IID9 */
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS); /* exact IID0 */
 assert(Link.Channels[0].Remote[1].Id==0&&Link.Channels[0].Remote[1].Busy);
 assert(CrGlinkServiceTx(&Link)==CR_SUCCESS);
 UINT32 End=Descriptor[1];assert(End>=32&&Tx[End-32]==9&&Tx[End-28]==0);
 Packet(14,77,0,NULL,0);assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 assert(!Link.Channels[0].Remote[1].Busy&&Link.Channels[0].Remote[1].Size==1024);
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS);
 Packet(6,77,0,NULL,0);assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 /* A hole must not shadow a later, busy duplicate ID. */
 UINT32 Many[4]={1024,11,1024,12};Packet(5,77,2,Many,sizeof(Many));
 assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS); /* hole now IID11 */
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS); /* IID12 */
 Packet(6,77,11,NULL,0);assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 Pair[1]=12;Packet(5,77,1,Pair,sizeof(Pair));
 assert(CrGlinkPoll(&Link)==CR_COMPROMISED_DATA);
 /* A valid advertisement larger than the local pool is fully framed and
    * consumed, including a frame beyond the transport scratch-buffer size. */
 static UINT32 Large[2056];
 Fixture();Packet(2,88,sizeof("fastrpcglink-apps-dsp"),"fastrpcglink-apps-dsp",sizeof("fastrpcglink-apps-dsp"));
 Packet(4,2,0,NULL,0);assert(CrGlinkPoll(&Link)==CR_SUCCESS);
 for(UINT32 N=0;N<1028;++N){Large[N*2]=1024;Large[N*2+1]=N;}
 Descriptor[2]=Descriptor[3]=sizeof(Rx)-24;
 Packet(5,88,1028,Large,sizeof(Large));assert(CrGlinkPoll(&Link)==CR_SUCCESS&&Descriptor[2]==Descriptor[3]&&Link.Ready);
 UINT32 Start=Descriptor[3];Packet(5,77,128,Large,128*8);
 UINT32 Published=Descriptor[3];Descriptor[3]=(Published-8)%sizeof(Rx);
 assert(CrGlinkPoll(&Link)==CR_SUCCESS&&Descriptor[2]==Start); /* no partial consume */
 Descriptor[3]=Published;assert(CrGlinkPoll(&Link)==CR_SUCCESS&&Descriptor[2]==Published);
 UINT32 Count=0;for(UINT32 N=0;N<CR_GLINK_REMOTE_INTENTS;++N)if(Link.Channels[0].Remote[N].Present)++Count;
 assert(Count==64&&Link.Channels[0].Remote[1].Id==0);
 for(UINT32 N=0;N<64;++N){assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS);assert(CrGlinkServiceTx(&Link)==CR_SUCCESS);Descriptor[0]=Descriptor[1];}
 assert(CrGlinkSend(&Link,Data,sizeof(Data))==CR_NOT_READY);
 Packet(14,77,0,NULL,0);assert(CrGlinkPoll(&Link)==CR_SUCCESS&&CrGlinkSend(&Link,Data,sizeof(Data))==CR_SUCCESS);
 puts("GLink IID0 exact use/reuse/remove, busy duplicate protection and complete oversized advertisements PASS");
 return 0;
}
