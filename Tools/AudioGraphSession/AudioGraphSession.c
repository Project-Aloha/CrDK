/* Scripted host consumer; NEVER connects to a physical ADSP. BSD-3-Clause */
#include "AudioGraphPeer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int Profile(CONST char *Path, CR_AUDIO_CAL_REQUEST *Request)
{
  FILE *File = fopen(Path, "r");
  char Line[256];
  UINT32 Seen = 0;
  if (File == NULL) {
    return 0;
  }
  while (fgets(Line, sizeof(Line), File) != NULL) {
    if (strchr(Line, '\n') == NULL && !feof(File)) {
      goto Bad;
    }
    char *Tokens[6], *Word = strtok(Line, " \t\r\n");
    unsigned Count = 0;
    UINT32 Values[4] = {0};
    if (Word == NULL || Word[0] == '#') {
      continue;
    }
    while (Word != NULL && Count < 6) {
      Tokens[Count++] = Word;
      Word = strtok(NULL, " \t\r\n");
    }
    if (Word != NULL || Count < 2 || Count > 5) {
      goto Bad;
    }
    for (unsigned Index = 1; Index < Count; ++Index) {
      char *End;
      errno = 0;
      unsigned long Value = strtoul(Tokens[Index], &End, 0);
      if (errno != 0 || *End != '\0' || Tokens[Index][0] == '-' || Value > 0xffffffffUL) {
        goto Bad;
      }
      Values[Index - 1] = (UINT32)Value;
    }
    if (strcmp(Tokens[0], "gkv") == 0 && Count == 3) {
      if (Request->GraphKeyCount == CR_AUDIO_CAL_MAX_KEYS) {
        goto Bad;
      }
      Request->GraphKeys[Request->GraphKeyCount++] = (CR_AUDIO_CAL_KEY){Values[0], Values[1]};
    } else if (strcmp(Tokens[0], "ckv") == 0 && Count == 3) {
      if (Request->CalibrationKeyCount == CR_AUDIO_CAL_MAX_KEYS) {
        goto Bad;
      }
      Request->CalibrationKeys[Request->CalibrationKeyCount++] =
          (CR_AUDIO_CAL_KEY){Values[0], Values[1]};
    } else if (strcmp(Tokens[0], "format") == 0 && Count == 4 && !(Seen & 1)) {
      Seen |= 1;
      Request->SampleRate = Values[0];
      Request->BitWidth = Values[1];
      Request->Channels = Values[2];
    } else if (strcmp(Tokens[0], "codec_dma") == 0 && Count == 4 && !(Seen & 2)) {
      Seen |= 2;
      Request->LpaifType = Values[0];
      Request->InterfaceIndex = Values[1];
      Request->ActiveChannelsMask = Values[2];
    } else if (strcmp(Tokens[0], "endpoint_tag") == 0 && Count == 2 && !(Seen & 4)) {
      Seen |= 4;
      Request->EndpointTag = Values[0];
    } else if (strcmp(Tokens[0], "transport") == 0 && Count == 5 && !(Seen & 8)) {
      Seen |= 8;
      Request->HostPort = Values[0];
      Request->OpenToken = Values[1];
      Request->CalibrationToken = Values[2];
      Request->EndpointToken = Values[3];
    } else {
      goto Bad;
    }
  }
  int Okay = !ferror(File) && Seen == 15;
  fclose(File);
  return Okay;
Bad:
  fclose(File);
  return 0;
}

int main(int Argc,char **Argv) {
  if(Argc!=3){fprintf(stderr,"Usage: AudioGraphSession ACDB_FILE PROFILE_TEXT\nSimulation only; no hardware transport.\n");return 2;}
  CR_AUDIO_CAL_REQUEST Request={0};if(!Profile(Argv[2],&Request))return 2;
  FILE *F=fopen(Argv[1],"rb");if(!F)return 2;
  if(fseek(F,0,SEEK_END)){fclose(F);return 2;}long N=ftell(F);
  if(N<=0||(unsigned long)N>0xffffffffUL||fseek(F,0,SEEK_SET)){fclose(F);return 2;}
  VOID *Data=malloc((size_t)N),*Work=malloc(CrAudioCalWorkspaceBytes());
  AUDIO_GRAPH_PEER *Peer=calloc(1,sizeof(*Peer));CR_AUDIO_GRAPH_SESSION *S=calloc(1,sizeof(*S));CR_AUDIO_GRAPH_STORAGE *Storage=calloc(1,sizeof(*Storage));
  if(!Data||!Work||!Peer||!S||!Storage)return 2;
  if(fread(Data,1,(size_t)N,F)!=(size_t)N)return 2;fclose(F);
  AudioPeerInit(Peer,1);CR_AUDIO_GRAPH_PORT Port={0};
  CR_STATUS Status=CrAudioGraphPortInit(&Port,Peer,0x700);
  if(Status==CR_SUCCESS)Status=CrAudioGraphInit(S,&Port,Peer,&Peer->Binding,&Peer->Epoch,10000,Data,(UINTN)N,&Request,Work,CrAudioCalWorkspaceBytes(),Storage);
  free(Data);free(Work);
  if(Status!=CR_SUCCESS){printf("{\"simulation\":true,\"admitted\":false,\"status\":\"0x%llx\"}\n",(unsigned long long)Status);AudioPeerDetach(Peer);free(Storage);free(S);free(Peer);return 1;}
  AudioPeerAttach(Peer,S);UINT64 Now=0;
  while(Now<10000&&S->Phase!=CrAudioGraphConfigured){CrAudioGraphTick(S,Now++);AudioPeerProgress(Peer);}
  if(S->Phase!=CrAudioGraphConfigured)return 3;
  CrAudioGraphRequestClose(S);
  while(Now<20000&&S->Phase!=CrAudioGraphEnded){CrAudioGraphTick(S,Now++);AudioPeerProgress(Peer);}
  if(S->Phase!=CrAudioGraphEnded||S->Cookie||S->GraphPossible||Peer->Cookie||Peer->Graphs)return 3;
  AudioPeerDetach(Peer);
  printf("{\"simulation\":true,\"configured_then_closed\":true,\"commands\":%u,\"wire_fragments\":%u,\"maximum_packet_bytes\":%u,\"next_token\":%llu,\"hardware_ready\":false}\n",S->Replies,Peer->Fragments,Peer->MaxPacket,(unsigned long long)Port.NextToken);
  free(Storage);free(S);free(Peer);return 0;
}
