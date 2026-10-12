/* Offline consumer of the production AudioCalCrLib plan compiler.
   Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioCalCrLib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CONST char *Names[] = {"ok",
                              "invalid-argument",
                              "malformed",
                              "not-found",
                              "policy-limit",
                              "workspace-too-small",
                              "unsupported-container",
                              "unsupported-persistence",
                              "unsupported-offloaded-data",
                              "unsupported-processor-domain",
                              "unsupported-amdb",
                              "unsupported-driver-properties",
                              "unsupported-endpoint",
                              "unsupported-graph-property",
                              "missing-calibration-key",
                              "missing-calibration",
                              "missing-module-map",
                              "unsupported-external-subgraph"};
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
static int Save(CONST char *Directory, CONST char *Name, CONST void *Data, size_t Bytes)
{
  char Path[4096];
  int Length = snprintf(Path, sizeof(Path), "%s/%s", Directory, Name);
  if (Length < 0 || (size_t)Length >= sizeof(Path)) {
    return 0;
  }
  FILE *File = fopen(Path, "wb");
  if (File == NULL) {
    return 0;
  }
  int Okay = fwrite(Data, 1, Bytes, File) == Bytes;
  return fclose(File) == 0 && Okay;
}
int main(int Argc, char **Argv)
{
  if (Argc != 4) {
    fprintf(stderr, "Usage: AudioCalPlan ACDB_FILE PROFILE_TEXT EXISTING_OUTPUT_DIR\nOffline "
                    "diagnostic only; no hardware or transport access.\n");
    return 2;
  }
  CR_AUDIO_CAL_REQUEST Request = {0};
  if (!Profile(Argv[2], &Request)) {
    fprintf(stderr, "Invalid explicit profile\n");
    return 2;
  }
  FILE *File = fopen(Argv[1], "rb");
  if (File == NULL) {
    return 2;
  }
  if (fseek(File, 0, SEEK_END) != 0) {
    fclose(File);
    return 2;
  }
  long Bytes = ftell(File);
  if (Bytes <= 0 || (unsigned long)Bytes > 0xffffffffUL || fseek(File, 0, SEEK_SET) != 0) {
    fclose(File);
    return 2;
  }
  void *Data = malloc((size_t)Bytes), *Workspace = malloc(CrAudioCalWorkspaceBytes());
  CR_AUDIO_CAL_PLAN *Plan = malloc(sizeof(*Plan));
  if (Data == NULL || Workspace == NULL || Plan == NULL) {
    fclose(File);
    free(Data);
    free(Workspace);
    free(Plan);
    return 2;
  }
  int Read = fread(Data, 1, (size_t)Bytes, File) == (size_t)Bytes;
  fclose(File);
  if (!Read) {
    free(Data);
    free(Workspace);
    free(Plan);
    return 2;
  }
  CR_AUDIO_CAL_RESULT Result = CrAudioCalBuildPlan(Data, (UINTN)Bytes, &Request, Workspace,
                                                   CrAudioCalWorkspaceBytes(), Plan);
  free(Data);
  free(Workspace);
  if (Result != CrAudioCalOk) {
    printf("{\"result\":\"%s\",\"code\":%u,\"packets_emitted\":false}\n", Names[Result],
           (unsigned)Result);
    free(Plan);
    return 1;
  }
  if (!Save(Argv[3], "graph-open.gpr", Plan->Open, Plan->OpenBytes) ||
      !Save(Argv[3], "initial-calibration.gpr", Plan->Calibration, Plan->CalibrationBytes) ||
      !Save(Argv[3], "endpoint-config.gpr", Plan->Endpoint, Plan->EndpointBytes) ||
      !Save(Argv[3], "driver-properties.bin", Plan->Driver, Plan->DriverBytes)) {
    fprintf(stderr, "Cannot write diagnostic output\n");
    free(Plan);
    return 2;
  }
  printf("{\"result\":\"ok\",\"offline_only\":true,\"subgraphs\":[");
  for (UINT32 Index = 0; Index < Plan->SubgraphCount; ++Index) {
    CR_AUDIO_CAL_SUBGRAPH *Sg = &Plan->Subgraphs[Index];
    printf("%s{\"id\":%u,\"routing_domain\":%u,\"flushable\":%u,\"type\":%u}", Index ? "," : "",
           Sg->Id, Sg->RoutingDomain, Sg->Flushable, Sg->Type);
  }
  printf("],\"modules\":[");
  for (UINT32 Index = 0; Index < Plan->ModuleCount; ++Index) {
    CR_AUDIO_CAL_MODULE *Module = &Plan->Modules[Index];
    printf("%s{\"subgraph\":%u,\"module\":%u,\"instance\":%u,\"container\":%u}", Index ? "," : "",
           Module->Subgraph, Module->Module, Module->Instance, Module->Container);
  }
  printf("],\"edges\":[");
  for (UINT32 Index = 0; Index < Plan->EdgeCount; ++Index) {
    printf("%s[%u,%u]", Index ? "," : "", Plan->Edges[Index].Source,
           Plan->Edges[Index].Destination);
  }
  printf("],\"endpoint_module\":%u,\"endpoint_instance\":%u,\"parameter_count\":%u,"
         "\"open_bytes\":%u,\"open_property_bytes\":%u,\"calibration_bytes\":%u,\"endpoint_bytes\":"
         "%u}\n",
         Plan->EndpointModule, Plan->EndpointInstance, Plan->ParameterCount, Plan->OpenBytes,
         Plan->OpenPropertyBytes, Plan->CalibrationBytes, Plan->EndpointBytes);
  free(Plan);
  return 0;
}
