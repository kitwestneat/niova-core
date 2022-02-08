module ctlplane/ncpc

replace common/serfClient => ../../common/Serf/client

replace niova/go-pumicedb-lib/common => ../../../common

replace common/httpClient => ../../common/HTTP/client

replace common/requestResponseLib => ../../common/RequestResponselib

replace common/clientAPI => ../../common/ClientAPI

go 1.17

require (
	github.com/sirupsen/logrus v1.8.1
	niova/go-pumicedb-lib/common v0.0.0-00010101000000-000000000000
)

require (
	common/clientAPI v0.0.0-00010101000000-000000000000 // indirect
	common/httpClient v0.0.0-00010101000000-000000000000 // indirect
	common/requestResponseLib v0.0.0-00010101000000-000000000000 // indirect
	common/serfClient v0.0.0-00010101000000-000000000000 // indirect
	github.com/armon/go-metrics v0.0.0-20180917152333-f0300d1749da // indirect
	github.com/google/btree v0.0.0-20180813153112-4030bb1f1f0c // indirect
	github.com/hashicorp/errwrap v1.0.0 // indirect
	github.com/hashicorp/go-immutable-radix v1.0.0 // indirect
	github.com/hashicorp/go-msgpack v0.5.3 // indirect
	github.com/hashicorp/go-multierror v1.1.0 // indirect
	github.com/hashicorp/go-sockaddr v1.0.0 // indirect
	github.com/hashicorp/golang-lru v0.5.0 // indirect
	github.com/hashicorp/logutils v1.0.0 // indirect
	github.com/hashicorp/memberlist v0.3.0 // indirect
	github.com/hashicorp/serf v0.9.6 // indirect
	github.com/miekg/dns v1.1.41 // indirect
	github.com/sean-/seed v0.0.0-20170313163322-e2103e2c3529 // indirect
	github.com/stretchr/testify v1.4.0 // indirect
	golang.org/x/net v0.0.0-20210410081132-afb366fc7cd1 // indirect
	golang.org/x/sys v0.0.0-20210330210617-4fbd30eecc44 // indirect
)
