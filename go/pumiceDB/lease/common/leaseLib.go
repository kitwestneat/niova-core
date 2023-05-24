package leaseLib

import (
	list "container/list"

	uuid "github.com/satori/go.uuid"
)

const (
	GET             int = 0
	PUT                 = 1
	LOOKUP              = 2
	REFRESH             = 3
	GET_VALIDATE        = 4
	LOOKUP_VALIDATE     = 5
	INVALID             = 0
	INPROGRESS          = 1
	EXPIRED             = 2
	AIU                 = 3
	GRANTED             = 4
	NULL                = 5
	SUCCESS             = 0
	FAILURE             = -1
)

type LeaseReq struct {
	Rncui     string
	Client    uuid.UUID
	Resource  uuid.UUID
	Operation int
}

type LeaderTS struct {
	LeaderTerm int64
	LeaderTime int64
}

type LeaseRes struct {
	Client     uuid.UUID
	Resource   uuid.UUID
	Status     int
	LeaseState int
	TTL        int
	TimeStamp  LeaderTS
}

type LeaseMeta struct {
	Resource   uuid.UUID
	Client     uuid.UUID
	Status     int
	LeaseState int
	TTL        int
	TimeStamp  LeaderTS
}

type LeaseInfo struct {
	LeaseMetaInfo LeaseMeta
	ListElement   *list.Element
}