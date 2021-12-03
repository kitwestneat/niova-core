package main

import (
	"errors"
	"flag"
	PumiceDBCommon "niova/go-pumicedb-lib/common"
	PumiceDBServer "niova/go-pumicedb-lib/server"
	"niovakv/niovakvlib"
	"pmdbServer/serfagenthandler"
	"os"
	"unsafe"
	"io/ioutil"
	"bufio"
	"strings"
	//"encoding/json"
	"strconv"
	defaultLogger "log"
	log "github.com/sirupsen/logrus"
)

/*
#include <stdlib.h>
#include <string.h>
*/
import "C"

var seqno = 0
// Use the default column family
var colmfamily = "PMDBTS_CF"

type pmdbServerHandler struct{
	raftUUID           string
        peerUUID           string
        logDir             string
        logLevel           string
        gossipClusterFile  string
	gossipClusterNodes []string
	aport		   string
	rport		   string
	addr		   string
	GossipData         map[string]string
	ConfigString       string
	ConfigData         []PeerConfigData
}
type PeerConfigData struct{
	ClientPort string
	Port       string
	IPAddr	   string
}

func main() {
	serverHandler := pmdbServerHandler{}
	nso, pErr := serverHandler.parseArgs()
	if pErr != nil {
		log.Println(pErr)
		return
	}

	switch serverHandler.logLevel{
		case "Info":
			log.SetLevel(log.InfoLevel)
		case "Trace":
			log.SetLevel(log.TraceLevel)
	}

	//Create log file
	err := PumiceDBCommon.InitLogger(serverHandler.logDir)
	if err != nil {
		log.Error("Error while initating logger ", err)
		os.Exit(1)
	}

	err = serverHandler.startSerfAgent()
	if err != nil {
		log.Fatal("Error while initializing serf agent ",err)
	}

	log.Info("Raft and Peer UUID: ", nso.raftUuid, " ", nso.peerUuid)
	/*
	   Initialize the internal pmdb-server-object pointer.
	   Assign the Directionary object to PmdbAPI so the apply and
	   read callback functions can be called through pmdb common library
	   functions.
	*/
	nso.pso = &PumiceDBServer.PmdbServerObject{
		ColumnFamilies: colmfamily,
		RaftUuid:       nso.raftUuid,
		PeerUuid:       nso.peerUuid,
		PmdbAPI:        nso,
                SyncWrites:     false,
                CoalescedWrite: true,
	}

	// Start the pmdb server
	err = nso.pso.Run()

	if err != nil {
		log.Error(err)
	}
}

func usage() {
	flag.PrintDefaults()
	os.Exit(0)
}

func (handler *pmdbServerHandler) parseArgs() (*NiovaKVServer, error) {

	var err error

	flag.StringVar(&handler.raftUUID, "r", "NULL", "raft uuid")
	flag.StringVar(&handler.peerUUID, "u", "NULL", "peer uuid")

	/* If log path is not provided, it will use Default log path.
           default log path: /tmp/<peer-uuid>.log
        */
	defaultLog := "/" + "tmp" + "/" + handler.peerUUID + ".log"
	flag.StringVar(&handler.logDir, "l", defaultLog, "log dir")
	flag.StringVar(&handler.logLevel,"ll","Info","Log level")
	flag.StringVar(&handler.gossipClusterFile,"g","NULL","Serf agent port")
	flag.Parse()

	nso := &NiovaKVServer{}
	nso.raftUuid = handler.raftUUID
	nso.peerUuid = handler.peerUUID

	if nso == nil {
		err = errors.New("Not able to parse the arguments")
	} else {
		err = nil
	}

	return nso, err
}

func (handler *pmdbServerHandler) readPMDBServerConfig() {
	folder := os.Getenv("NIOVA_LOCAL_CTL_SVC_DIR")
	files, err := ioutil.ReadDir(folder+"/")
	if err!= nil{
		return
	}

	for _,file := range files{
		if strings.Contains(file.Name(),".peer") {
			f,_ := os.Open(folder+"/"+file.Name())
			scanner := bufio.NewScanner(f)
			peerData := PeerConfigData{}
			handler.ConfigString += file.Name()[:len(file.Name())-5]+"/"
			for scanner.Scan() {
				text := scanner.Text()
				lastIndex := len(strings.Split(text," "))-1
				key := strings.Split(text," ")[0]
				value := strings.Split(text," ")[lastIndex]
				switch key{
					case "CLIENT_PORT":
						peerData.ClientPort = value
						handler.ConfigString += value+"/"
					case "IPADDR":
						peerData.IPAddr = value
						handler.ConfigString += value+"/"
					case "PORT":
						peerData.Port = value
						handler.ConfigString += value+"/"
				}
			}
			f.Close()
			handler.ConfigData = append(handler.ConfigData,peerData)
		}
	}
}

func (handler *pmdbServerHandler) readGossipClusterFile() error{
	f,err := os.Open(handler.gossipClusterFile)
	if err != nil {
		return err
	}
	scanner := bufio.NewScanner(f)
	for scanner.Scan(){
		text := scanner.Text()
		splitData := strings.Split(text," ")
		addr := splitData[0]
		aport := splitData[1]
		rport := splitData[2]
		uuid := splitData[3]
		if uuid == handler.peerUUID{
			handler.aport = aport
			handler.rport = rport
			handler.addr = addr
		} else {
			handler.gossipClusterNodes = append(handler.gossipClusterNodes, addr+":"+aport)
		}
	}
	log.Info("Cluster nodes : ",handler.gossipClusterNodes)
	log.Info("Node serf info : ",handler.addr,handler.aport,handler.rport)
	return nil
}


func  (handler *pmdbServerHandler) startSerfAgent() error {
	err := handler.readGossipClusterFile()
	if err != nil{
		return err
	}
	serfLog := "00"
	switch serfLog{
        case "ignore":
                defaultLogger.SetOutput(ioutil.Discard)
        default:
                f, err := os.OpenFile("serfLog.log", os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0644)
                if err != nil {
                        defaultLogger.SetOutput(os.Stderr)
                } else {
                        defaultLogger.SetOutput(f)
                }
	}

        //defaultLogger.SetOutput(ioutil.Discard)
	serfAgentHandler := serfagenthandler.SerfAgentHandler{
		Name: handler.peerUUID,
		BindAddr: handler.addr,
	}
        serfAgentHandler.BindPort, _ = strconv.Atoi(handler.aport)
        serfAgentHandler.AgentLogger = defaultLogger.Default()
	serfAgentHandler.RpcAddr = handler.addr
        serfAgentHandler.RpcPort = handler.rport
	//Start serf agent
	_, err = serfAgentHandler.Startup(handler.gossipClusterNodes, true)

	handler.readPMDBServerConfig()
	handler.GossipData = make(map[string]string)
	handler.GossipData["Type"] = "PMDB_SERVER"
	handler.GossipData["PC"] = handler.ConfigString
	handler.GossipData["RU"] = handler.raftUUID
	serfAgentHandler.SetTags(handler.GossipData)
	return err
}



type NiovaKVServer struct {
	raftUuid       string
	peerUuid       string
	columnFamilies string
	pso            *PumiceDBServer.PmdbServerObject
}

func (nso *NiovaKVServer) Apply(appId unsafe.Pointer, inputBuf unsafe.Pointer,
	inputBufSize int64, pmdbHandle unsafe.Pointer) {

	log.Trace("NiovaCtlPlane server: Apply request received")

	// Decode the input buffer into structure format
	applyNiovaKV := &niovakvlib.NiovaKV{}

	decodeErr := nso.pso.Decode(inputBuf, applyNiovaKV, inputBufSize)
	if decodeErr != nil {
		log.Error("Failed to decode the application data")
		return
	}

	log.Trace("Key passed by client: ", applyNiovaKV.InputKey)

	// length of key.
	keyLength := len(applyNiovaKV.InputKey)

	byteToStr := string(applyNiovaKV.InputValue)

	// Length of value.
	valLen := len(byteToStr)

	log.Trace("Write the KeyValue by calling PmdbWriteKV")
	nso.pso.WriteKV(appId, pmdbHandle, applyNiovaKV.InputKey,
		int64(keyLength), byteToStr,
		int64(valLen), colmfamily)

}

/*
func (nso *NiovaKVServer) Read(appId unsafe.Pointer, requestBuf unsafe.Pointer,
	requestBufSize int64, replyBuf unsafe.Pointer, replyBufSize int64) int64 {

	log.Trace("NiovaCtlPlane server: Read request received")


	decodeErr := nso.pso.Decode(inputBuf, applyNiovaKV, inputBufSize)
	if decodeErr != nil {
		log.Error("Failed to decode the application data")
		return
	}

	log.Trace("Key passed by client: ", applyNiovaKV.InputKey)

	// length of key.
	keyLength := len(applyNiovaKV.InputKey)

	byteToStr := string(applyNiovaKV.InputValue)

	// Length of value.
	valLen := len(byteToStr)

	log.Trace("Write the KeyValue by calling PmdbWriteKV")
	nso.pso.WriteKV(appId, pmdbHandle, applyNiovaKV.InputKey,
		int64(keyLength), byteToStr,
		int64(valLen), colmfamily)

}
*/
func (nso *NiovaKVServer) Read(appId unsafe.Pointer, requestBuf unsafe.Pointer,
	requestBufSize int64, replyBuf unsafe.Pointer, replyBufSize int64) int64 {

	log.Trace("NiovaCtlPlane server: Read request received")

	//Decode the request structure sent by client.
	reqStruct := &niovakvlib.NiovaKV{}
	decodeErr := nso.pso.Decode(requestBuf, reqStruct, requestBufSize)

	if decodeErr != nil {
		log.Error("Failed to decode the read request")
		return -1
	}

	log.Trace("Key passed by client: ", reqStruct.InputKey)

	keyLen := len(reqStruct.InputKey)
	log.Trace("Key length: ", keyLen)

	//Pass the work as key to PmdbReadKV and get the value from pumicedb
	readResult, readErr := nso.pso.ReadKV(appId, reqStruct.InputKey,
		int64(keyLen), colmfamily)
	var valType []byte
	var replySize int64
	var copyErr error

	if readErr == nil {
		valType = readResult
		inputVal := string(valType)
		log.Trace("Input value after read request:", inputVal)

		resultReq := niovakvlib.NiovaKV{
			InputKey:   reqStruct.InputKey,
			InputValue: valType,
		}

		//Copy the encoded result in replyBuffer
		replySize, copyErr = nso.pso.CopyDataToBuffer(resultReq, replyBuf)
		if copyErr != nil {
			log.Error("Failed to Copy result in the buffer: %s", copyErr)
			return -1
		}
	} else {
		log.Error(readErr)
	}

	log.Trace("Reply size: ", replySize)

	return replySize
}
