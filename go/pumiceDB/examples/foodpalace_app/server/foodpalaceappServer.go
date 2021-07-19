package main

import (
	"flag"
	"fmt"
	"foodpalaceapp.com/foodpalaceapplib"
	log "github.com/sirupsen/logrus"
	"niova/go-pumicedb-lib/server"
	"os"
	"strconv"
	"strings"
	"unsafe"
)

/*
#include <stdlib.h>
#include <string.h>
*/
import "C"

var (
	raftUuid string
	peerUuid string
	//Use the default column family
	colmfamily = "PMDBTS_CF"
	logDir     string
)

type FoodpalaceServer struct {
	raftUuid       string
	peerUuid       string
	columnFamilies string
	pso            *PumiceDBServer.PmdbServerObject
}

//Method to initizalize logger.
func (fpso *FoodpalaceServer) initLogger() {

	var filename string = logDir + "/" + fpso.peerUuid + ".log"
	// Create the log file if doesn't exist. And append to it if it already exists.
	f, err := os.OpenFile(filename, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0644)
	Formatter := new(log.TextFormatter)

	//Set Formatter.
	Formatter.TimestampFormat = "02-01-2006 15:04:05"
	Formatter.FullTimestamp = true
	log.SetFormatter(Formatter)

	if err != nil {
		// Cannot open log file. Logging to stderr
		fmt.Println(err)
	} else {
		log.SetOutput(f)
	}
	log.Info("peer:", fpso.peerUuid)
}

//Method for Apply callback.
func (fpso *FoodpalaceServer) Apply(appUuid unsafe.Pointer, dataBuf unsafe.Pointer,
	dataBufSz int64, pmdbHandle unsafe.Pointer) {

	data := &foodpalaceapplib.FoodpalaceData{}
	fpso.pso.Decode(dataBuf, data, dataBufSz)
	log.Info("Data received from client: ", data)

	//Convert resturant_id from int to string and store as fp_app_key.
	fpAppKey := strconv.Itoa(int(data.RestaurantId))
	appKeyLen := len(fpAppKey)

	//Lookup for the key if it is already present.
	prevValue, err := fpso.pso.LookupKey(fpAppKey, int64(appKeyLen), colmfamily)

	//If previous value is not null, update value of votes.
	if err == nil {

		//Split the prev_data_value.
		resData := strings.Split(prevValue, "_")

		//Take last parameter of res_data (votes) and convert it to int64.
		prevVotes, _ := strconv.ParseInt(resData[len(resData)-1], 10, 64)

		//Update votes by adding it with previous votes.
		data.Votes += prevVotes
	}

	fpAppValue := fmt.Sprintf("%s_%s_%s_%s_%d", data.RestaurantName, data.City, data.Cuisines, data.RatingsText, data.Votes)
	fmt.Println("fpAppValue", fpAppValue)
	appValLen := len(fpAppValue)

	//Write key,values.
	fpso.pso.WriteKV(appUuid, pmdbHandle, fpAppKey, int64(appKeyLen), fpAppValue,
		int64(appValLen), colmfamily)
}

//Method for read callback.
func (fpso *FoodpalaceServer) Read(appUuid unsafe.Pointer, dataReqBuf unsafe.Pointer,
	dataReqBufsz int64, dataReplyBuf unsafe.Pointer, dataReplyBufsz int64) int64 {

	var resultSplt []string
	log.Info("Read request received from client")

	//Decode the request structure sent by client.
	readReqData := &foodpalaceapplib.FoodpalaceData{}

	fpso.pso.Decode(dataReqBuf, readReqData, dataReqBufsz)

	log.Info("Key passed by client: ", readReqData.RestaurantId)

	//Typecast RestaurantId into string.
	fappKey := strconv.Itoa(int(readReqData.RestaurantId))
	fappKeyLen := len(fappKey)

	result, readErr := fpso.pso.ReadKV(appUuid, fappKey, int64(fappKeyLen), colmfamily)
	if readErr == nil {
		//Split the result to get respective values.
		resultSplt = strings.Split(result, "_")
	}

	votesInt64, err := strconv.ParseInt(resultSplt[4], 10, 64)
	if err != nil {
		log.Error(err)
		return -1
	}
	//Copy the result in data_reply_buf.
	replyData := foodpalaceapplib.FoodpalaceData{
		RestaurantId:   readReqData.RestaurantId,
		RestaurantName: resultSplt[0],
		City:           resultSplt[1],
		Cuisines:       resultSplt[2],
		RatingsText:    resultSplt[3],
		Votes:          votesInt64,
	}

	//Copy the encoded result in reply_buffer.
	dataReplySize, copyErr := fpso.pso.CopyDataToBuffer(replyData, dataReplyBuf)
	if copyErr != nil {
		log.Error("Failed to Copy result in the buffer: %s", copyErr)
		return -1
	}
	log.Info("length of buffer is:", dataReplySize)
	return dataReplySize
}

//Function to get commandline parameters and initizalize FoodpalaceServer instance.
func foodPalaceServerNew() *FoodpalaceServer {

	fpso := &FoodpalaceServer{}

	//Method call to accept cmdline parameters and start server.
	flag.StringVar(&fpso.raftUuid, "r", "NULL", "raft uuid")
	flag.StringVar(&fpso.peerUuid, "u", "NULL", "peer uuid")
	flag.StringVar(&logDir, "l", "./", "log directory path")
	flag.Parse()

	log.Info("Raft UUID: ", fpso.raftUuid)
	log.Info("Peer UUID: ", fpso.peerUuid)
	log.Info("Log Directory Path:", logDir)

	return fpso
}

//If log directory is not exist it creates directory.
//and if dir path is not passed then it will create log file in current directory by default.
func makeDirectoryIfNotExists() error {

	if _, err := os.Stat(logDir); os.IsNotExist(err) {
		return os.Mkdir(logDir, os.ModeDir|0755)
	}
	return nil
}

func main() {

	//Print help message.
	if len(os.Args) == 1 || os.Args[1] == "-help" || os.Args[1] == "--help" || os.Args[1] == "-h" {
		fmt.Println("\nUsage: \n   For help:             ./foodpalaceappserver [-h] \n   To start server:      ./foodpalaceappserver -r [raftUuid] -u [peerUuid]")
		fmt.Println("\nPositional Arguments: \n   -r    raftUuid \n   -u    peerUuid")
		fmt.Println("\nOptional Arguments: \n   -h, --help            show this help message and exit")
		os.Exit(0)
	}

	//Get Command line parameters and create FoodpalaceServer structure instance.
	fpso := foodPalaceServerNew()

	//Create log directory if not exists.
	makeDirectoryIfNotExists()

	//Initialize logger.
	fpso.initLogger()

	/*
	   Initialize the internal pmdb-server-object pointer.
	   Assign the Directionary object to PmdbAPI so the apply and
	   read callback functions can be called through pmdb common library
	   functions.
	*/
	fpso.pso = &PumiceDBServer.PmdbServerObject{
		ColumnFamilies: colmfamily,
		RaftUuid:       fpso.raftUuid,
		PeerUuid:       fpso.peerUuid,
		PmdbAPI:        fpso,
	}

	//Start the pmdb server.
	err := fpso.pso.Run()

	if err != nil {
		log.Error(err)
	}
}