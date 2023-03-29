package main

import (
	leaseClientLib "LeaseLib/leaseClient"
	"bytes"
	serviceDiscovery "common/clientAPI"
	leaseLib "common/leaseLib"
	"common/requestResponseLib"
	compressionLib "common/specificCompressionLib"
	"encoding/gob"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"math"
	"math/rand"
	PumiceDBCommon "niova/go-pumicedb-lib/common"
	"os"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"time"

	uuid "github.com/satori/go.uuid"
	log "github.com/sirupsen/logrus"
	maps "golang.org/x/exp/maps"
)

type clientHandler struct {
	requestKey         string
	requestValue       string
	raftUUID           string
	addr               string
	port               string
	operation          string
	configPath         string
	logPath            string
	resultFile         string
	rncui              string
	rangeQuery         bool
	relaxedConsistency bool
	count              int
	seed               int
	lastKey            string
	operationMetaObjs  []opData //For filling json data
	clientAPIObj       serviceDiscovery.ServiceDiscoveryHandler
	seqNum             uint64
	valSize            int
	serviceRetry       int
}

type request struct {
	Opcode    string      `json:"Operation"`
	Key       string      `json:"Key"`
	Value     interface{} `json:"Value"`
	Timestamp time.Time   `json:"Request_timestamp"`
}

type response struct {
	Status         int         `json:"Status"`
	ResponseValue  interface{} `json:"Response"`
	SequenceNumber uint64      `json:"Sequence_number"`
	validate       bool        `json:"validate"`
	Timestamp      time.Time   `json:"Response_timestamp"`
}

type opData struct {
	RequestData  request       `json:"Request"`
	ResponseData response      `json:"Response"`
	TimeDuration time.Duration `json:"Req_resolved_time"`
}

type multiWriteStatus struct {
	Status int
	Value  interface{}
}

type nisdData struct {
	UUID      uuid.UUID `json:"UUID"`
	Status    string    `json:"Status"`
	WriteSize string    `json:"WriteSize"`
}

func usage() {
	flag.PrintDefaults()
	os.Exit(0)
}

const letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

func randSeq(n int, r *rand.Rand) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = letters[r.Intn(len(letters))]
	}
	return b
}

// will be used to write to json file
type JsonLeaseReq struct {
	Client    uuid.UUID
	Resource  uuid.UUID
	Operation string
}

type JsonLeaseResp struct {
	Client     uuid.UUID
	Resource   uuid.UUID
	Status     string
	LeaseState string
	TTL        int
	TimeStamp  leaseLib.LeaderTS
}

type WriteObj struct {
	Request  JsonLeaseReq
	Response JsonLeaseResp
}

func getStringLeaseState(leaseState int) string {
	switch leaseState {
	case leaseLib.GRANTED:
		return "GRANTED"
	case leaseLib.INPROGRESS:
		return "IN-PROGRESS"
	case leaseLib.EXPIRED:
		return "EXPIRED"
	case leaseLib.AIU:
		return "ALREADY-IN-USE"
	case leaseLib.INVALID:
		return "INVALID"
	}
	return "UNKNOWN"
}

func getStringOperation(op int) string {
	switch op {
	case leaseLib.GET:
		return "GET"
	case leaseLib.PUT:
		return "PUT"
	case leaseLib.LOOKUP:
		return "LOOKUP"
	case leaseLib.REFRESH:
		return "REFRESH"
	}
	return "UNKNOWN"
}

func prepareLeaseJsonResponse(requestObj leaseLib.LeaseReq, responseObj leaseLib.LeaseRes) WriteObj {
	req := JsonLeaseReq{
		Client:    requestObj.Client,
		Resource:  requestObj.Resource,
		Operation: getStringOperation(requestObj.Operation),
	}
	resp := JsonLeaseResp{
		Client:     responseObj.Client,
		Resource:   responseObj.Resource,
		Status:     responseObj.Status,
		LeaseState: getStringLeaseState(responseObj.LeaseState),
		TTL:        responseObj.TTL,
		TimeStamp:  responseObj.TimeStamp,
	}
	res := WriteObj{
		Request:  req,
		Response: resp,
	}
	return res
}

func generateVdevRange(count int64, seed int64, valSize int) map[string][]byte {
	kvMap := make(map[string][]byte)
	r := rand.New(rand.NewSource(seed))
	var nodeUUID []string
	var vdevUUID []string
	nodeNisdMap := make(map[string][]string)
	//Node UUID
	/*
		FailureDomain
		Info
		State
		HostName
		NISD-UUIDs
	*/
	noUUID := count
	for i := int64(0); i < noUUID; i++ {
		randomNodeUUID := uuid.NewV4()
		nodeUUID = append(nodeUUID, randomNodeUUID.String())
		prefix := "node." + randomNodeUUID.String()

		//NISD-UUIDs
		for j := int64(0); j < noUUID; j++ {
			randUUID := uuid.NewV4()
			nodeNisdMap[randomNodeUUID.String()] = append(nodeNisdMap[randomNodeUUID.String()], randUUID.String())
		}
		kvMap[prefix+".NISD-UUIDs"], _ = json.Marshal(nodeNisdMap[randomNodeUUID.String()])

	}
	//NISD
	/*
		Node-UUID
		Config-Info
		Device-Type
		Device-Path
		Device-Status
		Device-Info
		Device-Size
		Provisioned-Size
		VDEV-UUID.Chunk-Number.Chunk-Component-UUID
	*/
	for _, node := range nodeUUID {
		for _, nisd := range nodeNisdMap[node] {
			prefix := "nisd." + nisd

			//Node-UUID
			kvMap[prefix+".Node-UUID"] = []byte(node)

			//Config-Info
			configInfo := prefix + ".Config-Info"
			kvMap[configInfo] = randSeq(valSize, r)

			//VDEV-UUID
			for j := int64(0); j < noUUID; j++ {
				randUUID := uuid.NewV4()
				partNodePrefix := prefix + "." + randUUID.String()
				kvMap[partNodePrefix] = randSeq(valSize, r)
				vdevUUID = append(vdevUUID, randUUID.String())
			}
		}
	}

	//Vdev
	/*
		User-Token
		Snapshots-Txn-Seqno
		Chunk-Number.Chunk-Component-UUID
	*/
	for i := int64(0); i < int64(len(vdevUUID)); i++ {
		prefix := "v." + vdevUUID[i]
		kvMap[prefix+".User-Token"] = randSeq(valSize, r)

		noChunck := count
		Cprefix := prefix + ".c"
		for j := int64(0); j < noChunck; j++ {
			randUUID := uuid.NewV4()
			Chunckprefix := Cprefix + strconv.Itoa(int(j)) + "." + randUUID.String()
			kvMap[Chunckprefix] = randSeq(valSize, r)
		}
	}
	return kvMap
}

func filterKVPrefix(kvMap map[string][]byte, prefix string) map[string][]byte {
	resultantMap := make(map[string][]byte)
	for key, value := range kvMap {
		if strings.HasPrefix(key, prefix) {
			resultantMap[key] = value
		}
	}

	return resultantMap
}

//Function to get command line parameters
func (handler *clientHandler) getCmdParams() {
	flag.StringVar(&handler.requestKey, "k", "", "Key - For ReadRange pass '<prefix>*' e.g. : -k 'vdev.*'")
	flag.StringVar(&handler.addr, "a", "127.0.0.1", "Addr value")
	flag.StringVar(&handler.port, "p", "1999", "Port value")
	flag.StringVar(&handler.requestValue, "v", "", "Value")
	flag.StringVar(&handler.raftUUID, "ru", "", "RaftUUID of the cluster to be queried")
	flag.StringVar(&handler.configPath, "c", "./gossipNodes", "gossip nodes config file path")
	flag.StringVar(&handler.logPath, "l", "/tmp/temp.log", "Log path")
	flag.StringVar(&handler.operation, "o", "rw", "Specify the opeation to perform")
	flag.StringVar(&handler.resultFile, "j", "json_output", "Path along with file name for the resultant json file")
	flag.StringVar(&handler.rncui, "u", uuid.NewV4().String()+":0:0:0:0", "RNCUI for request / Lookout uuid")
	flag.IntVar(&handler.count, "n", 1, "Write number of key/value pairs per key type (Default 1 will write the passed key/value)")
	flag.BoolVar(&handler.relaxedConsistency, "r", false, "Set this flag if range could be performed with relaxed consistency")
	flag.IntVar(&handler.seed, "s", 10, "Seed value")
	flag.IntVar(&handler.valSize, "vs", 512, "Random value generation size")
	flag.Uint64Var(&handler.seqNum, "S", math.MaxUint64, "Sequence Number for read")
	flag.IntVar(&handler.serviceRetry, "sr", 1, "how many times you want to retry to pick the server if proxy is not available")
	flag.Parse()
}

//Write to Json
func (cli *clientHandler) write2Json(toJson interface{}) {
	file, err := json.MarshalIndent(toJson, "", " ")
	err = ioutil.WriteFile(cli.resultFile+".json", file, 0644)
	if err != nil {
		log.Error("Error in writing output to the file : ", err)
	}
}

//Converts map[string][]byte to map[string]string
func convMapToStr(map1 map[string][]byte) map[string]string {
	map2 := make(map[string]string)

	for k, v := range map1 {
		map2[k] = string(v)
	}

	return map2
}

func fillOperationData(status int, operation string, key string, value interface{}, seqNo uint64) *opData {
	requestMeta := request{
		Opcode: operation,
		Key:    key,
		Value:  value,
	}

	responseMeta := response{
		SequenceNumber: seqNo,
		Status:         status,
		ResponseValue:  value,
	}

	operationObj := opData{
		RequestData:  requestMeta,
		ResponseData: responseMeta,
	}
	return &operationObj
}

func (cli *clientHandler) getNISDInfo() map[string]nisdData {
	data := cli.clientAPIObj.GetMembership()
	nisdDataMap := make(map[string]nisdData)
	for _, node := range data {
		if (node.Tags["Type"] == "LOOKOUT") && (node.Status == "alive") {
			for cuuid, value := range node.Tags {
				d_uuid, err := compressionLib.DecompressUUID(cuuid)
				if err == nil {
					CompressedStatus := value[0]
					//Decompress
					thisNISDData := nisdData{}
					thisNISDData.UUID, err = uuid.FromString(d_uuid)
					if err != nil {
						log.Error(err)
					}
					if string(CompressedStatus) == "1" {
						thisNISDData.Status = "Alive"
					} else {
						thisNISDData.Status = "Dead"
					}

					nisdDataMap[d_uuid] = thisNISDData
				}
			}
		}
	}
	return nisdDataMap
}

func prepareKVRequest(key string, value []byte, rncui string, operation int) []byte {
	var kvReqObj requestResponseLib.KVRequest
	kvReqObj.Operation = operation
	kvReqObj.Key = key
	kvReqObj.Value = value
	var pumiceRequestBytes bytes.Buffer
	err := PumiceDBCommon.PrepareAppPumiceRequest(kvReqObj, rncui, &pumiceRequestBytes)
	if err != nil {
		log.Error("Pumice request creation error : ", err)
	}
	return pumiceRequestBytes.Bytes()

}

func (clientObj *clientHandler) write() {
	kvMap := make(map[string][]byte)
	// Fill kvMap with key/val from user or generate keys/vals
	if clientObj.count > 1 {
		kvMap = generateVdevRange(int64(clientObj.count), int64(clientObj.seed), clientObj.valSize)
	} else {
		kvMap[clientObj.requestKey] = []byte(clientObj.requestValue)
	}

	operationStatSlice := make(map[string]*multiWriteStatus)
	var operationStat interface{}
	var mut sync.Mutex
	var wg sync.WaitGroup
	// Create a int channel of fixed size to enqueue max requests
	requestLimiter := make(chan int, 100)
	for key, val := range kvMap {
		wg.Add(1)
		requestLimiter <- 1
		go func(key string, val []byte) {
			defer func() {
				wg.Done()
				<-requestLimiter
			}()

			var appRequestObj requestResponseLib.KVRequest
			var responseObj requestResponseLib.KVResponse
			var responseBytes []byte

			// preparing appReqObj to write to jsonOutfile
			appRequestObj.Key = key
			appRequestObj.Value = val
			appRequestObj.Operation = requestResponseLib.KV_WRITE
			err := func() error {

				//Fill the request object
				pumiceRequestBytes := prepareKVRequest(key, val, uuid.NewV4().String()+":0:0:0:0", requestResponseLib.KV_WRITE)

				//Send the write request
				var err error
				responseBytes, err = clientObj.clientAPIObj.Request(pumiceRequestBytes, "", true)
				if err != nil {
					log.Error("Error while sending the request : ", err)
					return err
				}

				//Decode the request
				dec := gob.NewDecoder(bytes.NewBuffer(responseBytes))
				err = dec.Decode(&responseObj)
				if err != nil {
					log.Error("Decoding error : ", err)
					return err
				}

				return nil
			}()

			//Request status filler
			if clientObj.count == 1 {
				if err != nil {
					operationStat = fillOperationData(1, "write", appRequestObj.Key, err.Error(), 0)
				} else {
					operationStat = fillOperationData(responseObj.Status, "write", appRequestObj.Key, string(appRequestObj.Value), 0)
				}
				return
			}

			var operationStatMulti multiWriteStatus
			if err != nil {
				operationStatMulti = multiWriteStatus{
					Status: 1,
					Value:  err.Error(),
				}
			} else {
				operationStatMulti = multiWriteStatus{
					Status: responseObj.Status,
					Value:  string(val),
				}
			}

			mut.Lock()
			operationStatSlice[key] = &operationStatMulti
			operationStat = operationStatSlice
			mut.Unlock()
		}(key, val)
	}
	wg.Wait()
	clientObj.write2Json(operationStat)
}

func (clientObj *clientHandler) read() {
	//var pumiceReqObj PumiceDBCommon.PumiceRequest
	//var appRequestObj requestResponseLib.KVRequest
	var responseObj requestResponseLib.KVResponse

	err := func() error {
		//Fill the request obj and encode it
		pumiceRequestBytes := prepareKVRequest(clientObj.requestKey, []byte(""), "", requestResponseLib.KV_READ)

		//Send the request
		responseBytes, err := clientObj.clientAPIObj.Request(pumiceRequestBytes, "", false)
		if err != nil {
			log.Error("Error while sending the request : ", err)
			return err
		}

		//Decode the request
		dec := gob.NewDecoder(bytes.NewBuffer(responseBytes))
		err = dec.Decode(&responseObj)
		if err != nil {
			log.Error("Decoding error : ", err)
			return err
		}

		return nil
	}()

	var operationStat *opData
	if err == nil {
		operationStat = fillOperationData(responseObj.Status, "read", responseObj.Key, string(responseObj.ResultMap[responseObj.Key]), 0)
	} else {
		operationStat = fillOperationData(1, "read", responseObj.Key, err.Error(), 0)
	}

	clientObj.write2Json(operationStat)
}

func (clientObj *clientHandler) rangeRead() {
	var Prefix, Key string
	var Operation int
	var err error
	var appRequestObj requestResponseLib.KVRequest
	var seqNum uint64

	Prefix = clientObj.requestKey[:len(clientObj.requestKey)-1]
	Key = clientObj.requestKey[:len(clientObj.requestKey)-1]

	Operation = requestResponseLib.KV_RANGE_READ
	// get sequence number from arguments
	seqNum = clientObj.seqNum
	// Keep calling range request till ContinueRead is true
	resultMap := make(map[string][]byte)
	var count int

	for {
		rangeResponseObj := requestResponseLib.KVResponse{}
		appRequestObj.Prefix = Prefix
		appRequestObj.Key = Key
		appRequestObj.Operation = Operation
		appRequestObj.Consistent = !clientObj.relaxedConsistency
		appRequestObj.SeqNum = seqNum

		var pumiceRequestBytes bytes.Buffer
		err = PumiceDBCommon.PrepareAppPumiceRequest(appRequestObj, "", &pumiceRequestBytes)
		if err != nil {
			log.Error("Pumice request creation error : ", err)
			break
		}

		var responseBytes []byte

		//Send the range request
		responseBytes, err = clientObj.clientAPIObj.Request(pumiceRequestBytes.Bytes(), "", false)
		if err != nil {
			log.Error("Error while sending request : ", err)
		}

		if len(responseBytes) == 0 {
			err = errors.New("Key not found")
			log.Error("Empty response : ", err)
			break
		}
		// decode the responseObj
		dec := gob.NewDecoder(bytes.NewBuffer(responseBytes))
		err = dec.Decode(&rangeResponseObj)
		if err != nil {
			log.Error("Decoding error : ", err)
			break
		}

		// copy result to global result variable
		maps.Copy(resultMap, rangeResponseObj.ResultMap)
		count += 1

		//Change sequence number and key for next iteration
		seqNum = rangeResponseObj.SeqNum
		Key = rangeResponseObj.Key
		if !rangeResponseObj.ContinueRead {
			break
		}
	}

	//Fill the json
	var operationStat *opData
	if err == nil {
		strResultMap := convMapToStr(resultMap)
		operationStat = fillOperationData(0, "range", appRequestObj.Key, strResultMap, seqNum)

		//Validate the range output
		fmt.Println("Generate the Data for read validation")
		genKVMap := generateVdevRange(int64(clientObj.count), int64(clientObj.seed), clientObj.valSize)

		// Get the expected data for read operation and compare against the output.
		tPrefix := clientObj.requestKey[:len(clientObj.requestKey)-1]
		filteredMap := filterKVPrefix(genKVMap, tPrefix)

		compare := reflect.DeepEqual(resultMap, filteredMap)
		if !compare {
			fmt.Println("Range verification read failure")
		}
		fmt.Println("The range query was completed in", count, "iterations")

	} else {
		operationStat = fillOperationData(1, "range", appRequestObj.Key, err.Error(), seqNum)
	}

	clientObj.write2Json(operationStat)
}

func (clientObj *clientHandler) writeToLeaseOutfile(leaseReq leaseLib.LeaseReq, responseObj leaseLib.LeaseRes) {
	res := prepareLeaseJsonResponse(leaseReq, responseObj)
	clientObj.write2Json(res)

}

func (clientObj *clientHandler) prepareLeaseHandlers(leaseReqHandler *leaseClientLib.LeaseClientReqHandler) error {
	raft, err := uuid.FromString(clientObj.raftUUID)
	if err != nil {
		log.Error("Error getting raft UUID ", err)
		return err
	}

	leaseClientObj := leaseClientLib.LeaseClient{
		RaftUUID:            raft,
		ServiceDiscoveryObj: &clientObj.clientAPIObj,
	}

	leaseReqHandler.LeaseClientObj = &leaseClientObj
	return err

}

func (clientObj *clientHandler) fillLeaseReqObj(leaseReq *leaseLib.LeaseReq, operation int) error {
	var err error

	if operation != leaseLib.LOOKUP {
		leaseReq.Client, err = uuid.FromString(clientObj.requestKey)
		if err != nil {
			log.Error(err)
			return err
		}
	}
	leaseReq.Resource, err = uuid.FromString(clientObj.requestValue)
	if err != nil {
		log.Error(err)
		return err
	}
	leaseReq.Operation = operation

	return err
}

func isRangeRequest(requestKey string) bool {
	return requestKey[len(requestKey)-1:] == "*"
}

func isSingleWriteReqValid(cli *clientHandler) bool {
	if cli.operation == "write" && cli.count == 1 && cli.requestValue == "" {
		return false
	}

	return true
}

func main() {
	//Intialize client object
	clientObj := clientHandler{}

	//Get commandline parameters.
	clientObj.getCmdParams()
	flag.Usage = usage
	if flag.NFlag() == 0 || !isSingleWriteReqValid(&clientObj) {
		usage()
		os.Exit(-1)
	}

	//Create logger
	err := PumiceDBCommon.InitLogger(clientObj.logPath)
	if err != nil {
		log.Error("Error while initializing the logger  ", err)
	}

	log.Info("----START OF EXECUTION---")

	//Init service discovery
	clientObj.clientAPIObj = serviceDiscovery.ServiceDiscoveryHandler{
		HTTPRetry: 10,
		SerfRetry: 5,
		RaftUUID:  clientObj.raftUUID,
	}
	stop := make(chan int)
	go func() {
		err := clientObj.clientAPIObj.StartClientAPI(stop, clientObj.configPath)
		if err != nil {
			operationStat := fillOperationData(-1, "setup", "", err.Error(), 0)
			clientObj.write2Json(operationStat)
			log.Error(err)
			os.Exit(1)
		}
	}()
	clientObj.clientAPIObj.TillReady("", clientObj.serviceRetry)
	if err != nil {
		operationStat := fillOperationData(-1, "setup", "", err.Error(), 0)
		clientObj.write2Json(operationStat)
		log.Error(err)
		os.Exit(1)
	}
	var passNext bool
	switch clientObj.operation {
	case "rw":
		log.Info("Defaulting to write and read")
		clientObj.operation = "write"
		clientObj.write()
		clientObj.operation = "read"
		clientObj.read()

	case "write":
		clientObj.clientAPIObj.TillReady("PROXY", clientObj.serviceRetry)
		if err != nil {
			log.Error(err)
			os.Exit(1)
		}

		clientObj.write()

	case "read":
		clientObj.clientAPIObj.TillReady("PROXY", clientObj.serviceRetry)
		if err != nil {
			log.Error(err)
			os.Exit(1)
		}

		if !isRangeRequest(clientObj.requestKey) {
			clientObj.read()
		} else {
			clientObj.rangeRead()
		}

	case "config":
		responseBytes, err := clientObj.clientAPIObj.GetPMDBServerConfig()
		log.Info("Response : ", string(responseBytes))
		if err != nil {
			log.Error("Unable to get the config data")
		}
		_ = ioutil.WriteFile(clientObj.resultFile+".json", responseBytes, 0644)

	case "membership":
		toJson := clientObj.clientAPIObj.GetMembership()
		file, _ := json.MarshalIndent(toJson, "", " ")
		_ = ioutil.WriteFile(clientObj.resultFile+".json", file, 0644)

	case "general":
		fmt.Printf("\033[2J")
		fmt.Printf("\033[2;0H")
		fmt.Print("UUID")
		fmt.Printf("\033[2;38H")
		fmt.Print("Type")
		fmt.Printf("\033[2;50H")
		fmt.Println("Status")
		offset := 3
		for {
			lineCounter := 0
			data := clientObj.clientAPIObj.GetMembership()
			for _, node := range data {
				currentLine := offset + lineCounter
				fmt.Print(node.Name)
				fmt.Printf("\033[%d;38H", currentLine)
				fmt.Print(node.Tags["Type"])
				fmt.Printf("\033[%d;50H", currentLine)
				fmt.Println(node.Status)
				lineCounter += 1
			}
			time.Sleep(2 * time.Second)
			fmt.Printf("\033[3;0H")
			for i := 0; i < lineCounter; i++ {
				fmt.Println("                                                       ")
			}
			fmt.Printf("\033[3;0H")
		}
	case "nisd":
		fmt.Printf("\033[2J")
		fmt.Printf("\033[2;0H")
		fmt.Println("NISD_UUID")
		fmt.Printf("\033[2;38H")
		fmt.Print("Status")
		fmt.Printf("\033[2;45H")
		fmt.Println("Parent_UUID(Lookout)")
		offset := 3
		for {
			lineCounter := 0
			data := clientObj.clientAPIObj.GetMembership()
			for _, node := range data {
				if (node.Tags["Type"] == "LOOKOUT") && (node.Status == "alive") {
					for uuid, value := range node.Tags {
						if uuid != "Type" {
							currentLine := offset + lineCounter
							fmt.Print(uuid)
							fmt.Printf("\033[%d;38H", currentLine)
							fmt.Print(strings.Split(value, "_")[0])
							fmt.Printf("\033[%d;45H", currentLine)
							fmt.Println(node.Name)
							lineCounter += 1
						}
					}
				}
			}
			time.Sleep(2 * time.Second)
			fmt.Printf("\033[3;0H")
			for i := 0; i < lineCounter; i++ {
				fmt.Println("                                                       ")
			}
			fmt.Printf("\033[3;0H")
		}

	case "Gossip":
		passNext = true

	case "NISDGossip":
		nisdDataMap := clientObj.getNISDInfo()
		fileData, _ := json.MarshalIndent(nisdDataMap, "", " ")
		ioutil.WriteFile(clientObj.resultFile+".json", fileData, 0644)
		if !passNext {
			break
		}
		fallthrough

	case "PMDBGossip":
		fileData, err := clientObj.clientAPIObj.GetPMDBServerConfig()
		if err != nil {
			log.Error("Error while getting pmdb server config data : ", err)
			break
		}
		if passNext {
			f, _ := os.OpenFile(clientObj.resultFile+".json", os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0600)
			f.WriteString(string(fileData))
			break
		}
		ioutil.WriteFile(clientObj.resultFile+".json", fileData, 0644)

	case "ProxyStat":
		clientObj.clientAPIObj.ServerChooseAlgorithm = 2
		clientObj.clientAPIObj.UseSpecificServerName = clientObj.requestKey
		responseBytes, err := clientObj.clientAPIObj.Request(nil, "/stat", false)
		if err != nil {
			log.Error("Error while sending request to proxy : ", err)
		}
		ioutil.WriteFile(clientObj.resultFile+".json", responseBytes, 0644)

	case "LookoutInfo":
		clientObj.clientAPIObj.ServerChooseAlgorithm = 2
		clientObj.clientAPIObj.UseSpecificServerName = clientObj.rncui
		//Request obj
		var appRequestObj requestResponseLib.LookoutRequest

		//Parse UUID
		appRequestObj.UUID, _ = uuid.FromString(clientObj.requestKey)
		appRequestObj.Cmd = clientObj.requestValue

		var appRequestBytes bytes.Buffer
		enc := gob.NewEncoder(&appRequestBytes)
		err := enc.Encode(appRequestObj)
		if err != nil {
			log.Error("Encoding error : ", err)
		}

		//TODO: Why PumiceRequest wrapper for lookout request;
		//There is no interaction of pumice layer in lookout request
		var pumiceReqObj PumiceDBCommon.PumiceRequest
		pumiceReqObj.ReqType = PumiceDBCommon.APP_REQ
		pumiceReqObj.ReqPayload = appRequestBytes.Bytes()

		var pumiceRequestBytes bytes.Buffer
		pumiceEnc := gob.NewEncoder(&pumiceRequestBytes)
		err = pumiceEnc.Encode(pumiceReqObj)
		if err != nil {
			log.Error("Encoding error : ", err)
			return
		}

		responseBytes, err := clientObj.clientAPIObj.Request(pumiceRequestBytes.Bytes(), "/v1/", false)

		if err != nil {
			log.Error("Error while sending request to proxy : ", err)
		}
		ioutil.WriteFile(clientObj.resultFile+".json", responseBytes, 0644)

	case "GetLease":
		clientObj.clientAPIObj.TillReady("PROXY", clientObj.serviceRetry)

		var leaseReqHandler leaseClientLib.LeaseClientReqHandler
		err = clientObj.prepareLeaseHandlers(&leaseReqHandler)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.InitLeaseReq(clientObj.requestValue, clientObj.requestKey, "", leaseLib.GET)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.LeaseOperationOverHTTP()
		if err != nil {
			log.Error(err)
			break
		}

		clientObj.writeToLeaseOutfile(leaseReqHandler.LeaseReq, leaseReqHandler.LeaseRes)
	case "LookupLease":
		clientObj.clientAPIObj.TillReady("PROXY", clientObj.serviceRetry)

		var leaseReqHandler leaseClientLib.LeaseClientReqHandler
		err = clientObj.prepareLeaseHandlers(&leaseReqHandler)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.InitLeaseReq(clientObj.requestValue, clientObj.requestKey, "", leaseLib.LOOKUP)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.LeaseOperationOverHTTP()
		if err != nil {
			log.Error(err)
			break
		}

		clientObj.writeToLeaseOutfile(leaseReqHandler.LeaseReq, leaseReqHandler.LeaseRes)
	case "RefreshLease":
		clientObj.clientAPIObj.TillReady("PROXY", clientObj.serviceRetry)

		var leaseReqHandler leaseClientLib.LeaseClientReqHandler
		err = clientObj.prepareLeaseHandlers(&leaseReqHandler)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.InitLeaseReq(clientObj.requestValue, clientObj.requestKey, "", leaseLib.REFRESH)
		if err != nil {
			log.Error(err)
			break
		}
		err = leaseReqHandler.LeaseOperationOverHTTP()
		if err != nil {
			log.Error(err)
			break
		}

		clientObj.writeToLeaseOutfile(leaseReqHandler.LeaseReq, leaseReqHandler.LeaseRes)

	}

}
