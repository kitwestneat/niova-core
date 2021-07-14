package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"math/rand"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	log "github.com/sirupsen/logrus"

	"niovakv/httpclient"
	"niovakv/niovakvlib"
	"niovakv/serfclienthandler"
)

var (
	ClientHandler                                                  serfclienthandler.SerfClientHandler
	config_path, operation, key, value, logPath, serial, noRequest string
	reqobjs_write, reqobjs_read                                    []niovakvlib.NiovaKV
	respFillerLock                                                 sync.Mutex
	operationMetaObjs                                              []opData //For filling json
	w                                                              sync.WaitGroup
	requestSentCount, timeoutCount                                 *int32
)

type request struct {
	Opcode    string    `json:"Operation"`
	Key       string    `json:"Key"`
	Value     string    `json:"Value"`
	Sent_to   string    `json:Sent_to`
	Timestamp time.Time `json:"Request_timestamp"`
}
type response struct {
	Status        int       `json:"Status"`
	ResponseValue string    `json:"Response"`
	Timestamp     time.Time `json:"Response_timestamp"`
}
type opData struct {
	RequestData  request       `json:"Request"`
	ResponseData response      `json:"Response"`
	TimeDuration time.Duration `json:"Req_resolved_time"`
}

//Create logfile for client.
func initLogger() {

	var filename string = logPath + "/" + "niovakvClient" + ".log"

	//Create the log file if doesn't exist. And append to it if it already exists.i
	f, err := os.OpenFile(filename, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0644)
	Formatter := new(log.TextFormatter)

	//Set Timestamp format for logfile.
	Formatter.TimestampFormat = "02-01-2006 15:04:05"
	Formatter.FullTimestamp = true
	log.SetFormatter(Formatter)

	if err != nil {
		// Cannot open log file. Logging to stderr
		log.Error(err)
	} else {
		log.SetOutput(f)
	}
}

//Function to get command line parameters while starting of the client.
func getCmdParams() {
	flag.StringVar(&config_path, "c", "./", "config file path")
	flag.StringVar(&logPath, "l", ".", "log file path")
	flag.StringVar(&operation, "o", "NULL", "write/read operation")
	flag.StringVar(&key, "k", "NULL", "Key")
	flag.StringVar(&value, "v", "NULL", "Value")
	flag.StringVar(&serial, "s", "no", "Serialized request or not")
	flag.StringVar(&noRequest, "n", "5", "No of request")
	flag.Parse()
}

func sendReq(req *niovakvlib.NiovaKV, addr string, port string, write bool) {
	atomic.AddInt32(requestSentCount, 1)
	requestMeta := request{
		Opcode:    req.InputOps,
		Key:       req.InputKey,
		Value:     string(req.InputValue),
		Sent_to:   addr + ":" + port,
		Timestamp: time.Now(),
	}
	var resp *niovakvlib.NiovaKVResponse
	if write {
		resp, _ = httpclient.PutRequest(req, addr, port)
	} else {
		resp, _ = httpclient.GetRequest(req, addr, port)
	}
	responseMeta := response{
		Timestamp:     time.Now(),
		Status:        resp.RespStatus,
		ResponseValue: string(resp.RespValue),
	}
	operationObj := opData{
		RequestData:  requestMeta,
		ResponseData: responseMeta,
		TimeDuration: responseMeta.Timestamp.Sub(requestMeta.Timestamp),
	}
	if responseMeta.Status == -1 {
		atomic.AddInt32(timeoutCount, 1)
	}
	respFillerLock.Lock()
	operationMetaObjs = append(operationMetaObjs, operationObj)
	respFillerLock.Unlock()
}

//Get any client addr
func getServerAddr(refresh bool) (string, string) {
	if refresh {
		ClientHandler.GetData(false)
	}
	//Get random addr
	randomIndex := rand.Intn(len(ClientHandler.Agents))
	node := ClientHandler.Agents[randomIndex]
	return ClientHandler.AgentData[node].Addr, ClientHandler.AgentData[node].Tags["Hport"]
}

func main() {
	var count1, count3 int32
	count1 = 0
	count3 = 0
	requestSentCount = &count1
	timeoutCount = &count3
	//Get commandline parameters.
	getCmdParams()

	//Create log file.
	initLogger()

	//For serf client init
	ClientHandler = serfclienthandler.SerfClientHandler{}
	ClientHandler.AgentData = make(map[string]*serfclienthandler.Data)
	ClientHandler.Initdata(config_path)
	ClientHandler.Retries = 5

	//Create Write and read request
	n, _ := strconv.Atoi(noRequest)
	for i := 0; i < n; i++ {
		reqObj1 := niovakvlib.NiovaKV{}
		reqObj1.InputOps = "write"
		reqObj1.InputKey = "Key" + strconv.Itoa(i)
		reqObj1.InputValue = []byte("Asia" + strconv.Itoa(i))
		reqobjs_write = append(reqobjs_write, reqObj1)

		reqObj2 := niovakvlib.NiovaKV{}
		reqObj2.InputOps = "read"
		reqObj2.InputKey = "Key" + strconv.Itoa(i)
		reqobjs_read = append(reqobjs_read, reqObj2)
	}

	//Send request
	refresh := true
	addr, port := getServerAddr(refresh)
	for j := 0; j < n; j++ {
		addr, port := getServerAddr(false)
		if serial == "no" {
			w.Add(1)
			go func(index int) {
				sendReq(&reqobjs_write[index], addr, port, true)
				w.Done()
			}(j)
		} else {
			sendReq(&reqobjs_write[j], addr, port, true)
		}
	}

	//Wait till all write are completed
	fmt.Println("Waiting for writes to complete")
	w.Wait()
	fmt.Println("Writes completed")
	fmt.Println("No of request sent, no of time outs : ", count1, count3)
	count1 = 0
	count3 = 0

	for j := n - 1; j >= 0; j-- {
		addr, port = getServerAddr(false)
		if serial == "no" {
			w.Add(1)
			go func(index int) {
				sendReq(&reqobjs_read[index], addr, port, false)
				w.Done()
			}(j)
		} else {
			sendReq(&reqobjs_read[j], addr, port, false)
		}
	}

	//Wait till all reads are completed
	fmt.Println("Waiting for read to complete")
	w.Wait()
	fmt.Println("Reads completed")
	fmt.Println("No of request sent, no of time outs : ", count1, count3)

	//Find avg response time
	Writesum := 0
	Readsum := 0
	for _, ops := range operationMetaObjs {
		if ops.RequestData.Opcode == "write" {
			Writesum += int(ops.TimeDuration.Seconds())
		} else {
			Readsum += int(ops.TimeDuration.Seconds())
		}
	}
	fmt.Println("Avg write response time : ", Writesum/n, "sec")
	fmt.Println("Avg read response time : ", Readsum/n, "sec")
	fmt.Println("Avg response time : ", (Writesum+Readsum)/(2*n), "sec")
	file, _ := json.MarshalIndent(operationMetaObjs, "", " ")
	_ = ioutil.WriteFile("operation.json", file, 0644)

}
