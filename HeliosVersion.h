//-------------------------------------------------------------------------------------------------
//
//  HeliosVersion.h
//  heliosAgent
//
//  Identity constants reported by the `hello` verb. Bump HELIOS_PROTOCOL_VERSION
//  only on a breaking wire-protocol change (see PROTOCOL.md).
//
//-------------------------------------------------------------------------------------------------

#ifndef HELIOS_VERSION_H
#define HELIOS_VERSION_H

#define HELIOS_AGENT_NAME        "heliosAgent"
// 0.2.0: sysinfo verb (additive; protocol unchanged -- old agents answer
// "unknown verb" and the Mac side degrades gracefully).
#define HELIOS_VERSION           "0.2.0"
#define HELIOS_PROTOCOL_VERSION  1

#endif
