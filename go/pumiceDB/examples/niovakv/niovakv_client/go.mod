<<<<<<< HEAD
<<<<<<< HEAD
module niovakv/clientapi
=======
<<<<<<< HEAD
module niovakv/niovakv_client
=======
module niovakv/clientapi
>>>>>>> changed layout of niovakv_client directory
>>>>>>> changed layout of niovakv_client directory
=======
module niovakv/clientapi
>>>>>>> rebased and getting niovakv_client to work with api again

replace niovakv/serfclienthandler => ../serf/client

replace niova/go-pumicedb-lib/common => ../../../common

replace niovakv/httpclient => ../http/client

replace niovakv/niovakvlib => ../lib

go 1.16

require (
	github.com/sirupsen/logrus v1.8.1
	niovakv/httpclient v0.0.0-00010101000000-000000000000
	niovakv/niovakvlib v0.0.0-00010101000000-000000000000
	niovakv/serfclienthandler v0.0.0-00010101000000-000000000000
)
