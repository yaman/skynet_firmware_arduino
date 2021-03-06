
#include <Skynetclient.h>

#define TOKEN_STRING(js, t, s) \
	(strncmp(js+(t).start, s, (t).end - (t).start) == 0 \
	 && strlen(s) == (t).end - (t).start)
	
ringbuffer txbuf(SKYNET_TX_BUFFER_SIZE);
ringbuffer rxbuf(SKYNET_RX_BUFFER_SIZE);

SkynetClient::SkynetClient(Client &_client){
	this->client = &_client; 
}

int SkynetClient::connect(const char* host, uint16_t port) 
{
	thehost = host;
	status = 0;
	bind = 0;

	//connect tcp or fail
	if (!client->connect(host, port)) 
		return false;

	//establish socket or fail
	sendHandshake();
	if(!readHandshake()){
		stop();
		return false;
	}
	
	//monitor to initiate communications with skynet TODO some fail condition
	while(!monitor());

	//havent gotten a heartbeat yet so lets set current time
	lastBeat = millis();

	return status;
}

uint8_t SkynetClient::connected() {
  return bind;
}

void SkynetClient::stop() {
	status = 0;
	bind = 0;
	client->stop();
}

int SkynetClient::monitor()
{
	//if we've expired, reconnect to skynet at least
	if(status == 1 && (unsigned long)(millis() - lastBeat) >= HEARTBEATTIMEOUT){
		DBGC(F("Timeout: "));
		DBGCN(millis());

		DBGC(F("lastbeat: "));
		DBGCN(lastBeat);

		stop();
		return status;
	}

	flush();

    if (client->available()) 
    {
		int size = readLineSocket();

		char *first  = strchr(databuffer, ':'); 
		char *second  = strchr(first+1, ':');
		char *third  = strchr(second+1, ':');

		//-2 for the colons
		int ackSize = second - first - 2;
		char ack[MAXACK+1];

		//if first and second colon aren't next to eachother, and acksize is sane, grab the ack character(s)
		if (ackSize>0 && ackSize<MAXACK)
		{
			DBGCN(ackSize);
			DBGCN(first+1);

			strncpy(ack, first+1, ackSize);
			ack[ackSize] = '\0';
			DBGC(F("ack: "));
			DBGCN(ack);
		}

		//where json parser should start
		char *dataptr = third+1;

		char socketType = databuffer[0];
		switch (socketType) {
	
			//disconnect
			case '0':
				DBGCN(F("Disconnect"));
				stop();
				break;
			
			//messages
			case '1':
				DBGCN(F("Socket Connect"));
				break;
				
			case '3':
				DBGCN(F("Data"));
				b64::decodestore(dataptr, rxbuf);
				break;

			case '5':	
				DBGCN(F("Message"));
				processSkynet(dataptr, ack);
				break;
				
			//hearbeat
			case '2':
				DBGC(F("Heartbeat at: "));
				lastBeat = millis();
				DBGCN(lastBeat);
				client->print((char)0);
				client->print(F("2::"));
				client->print((char)255);
				break;

		    //huh?
			default:
				DBGC(F("Drop: "));
				DBGCN(socketType);
				break;
		}
	}
	return status;
}

//Got credentials back, store if necessary
void SkynetClient::processIdentify(char *data, jsmntok_t *tok)
{
	char token[TOKENSIZE];
	char uuid[UUIDSIZE];

    DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);

	printByByte("{\"name\":\"identity\",\"args\":[{\"socketid\":\"");
	printToken(data, tok[7]);
	
	if( eeprom_read_byte( (uint8_t*)EEPROMBLOCKADDRESS) == EEPROMBLOCK )
	{
		getToken(token);

		getUuid(uuid);

		printByByte("\", \"uuid\":\"");
		printByByte(uuid);

		printByByte("\", \"token\":\"");
		printByByte(token);
	}
	printByByte("\"}]}");
  
	DBGCN((char)255);
	client->print((char)255);
}

//lookup uuid and token if we have them and send in for validation
void SkynetClient::processReady(char *data, jsmntok_t *tok)
{
	DBGCN(F("Skynet Connect"));

	char token[TOKENSIZE];
	char uuid[UUIDSIZE];

	status = 1;
	
    getToken(token);
	
    //if token has been refreshed, save it
    if (!TOKEN_STRING(data, tok[15], token ))
    {
		DBGCN(F("token refresh"));
	  	strncpy(token, data + tok[15].start, tok[15].end - tok[15].start);

        DBGC(F("new: "));
      	DBGCN(token);
      
	  	eeprom_write_bytes(TOKENADDRESS, token, TOKENSIZE);

		//write block identifier, arduino should protect us from writing if it doesnt need it?
      	eeprom_write_byte((uint8_t *)EEPROMBLOCKADDRESS, (uint8_t)EEPROMBLOCK); 

    }else
    {
		DBGCN(F("no token refresh necessary"));
    }
	
	getUuid(uuid);

    //if uuid has been refreshed, save it
    if (!TOKEN_STRING(data, tok[13], uuid ))
    {
      	DBGCN(F("uuid refresh"));
		strncpy(uuid, data + tok[13].start, tok[13].end - tok[13].start);
		
      	DBGC(F("new: "));
      	DBGCN(uuid);
		
      	eeprom_write_bytes(UUIDADDRESS, uuid, UUIDSIZE);
		
		//write block identifier, arduino should protect us from writing if it doesnt need it?
      	eeprom_write_byte((uint8_t *)EEPROMBLOCKADDRESS, (uint8_t)EEPROMBLOCK); 

     }else
     {
       	DBGCN(F("no uuid refresh necessary"));
     }
}

//Credentials have been invalidted, send blank identify for new ones
void SkynetClient::processNotReady(char *data, jsmntok_t *tok)
{
    DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);

	printByByte("{\"name\":\"identity\",\"args\":[{\"socketid\":\"");
	printToken(data, tok[11]);
	printByByte("\"}]}");
  
	DBGCN((char)255);
	client->print((char)255);
}

void SkynetClient::processBind(char *data, jsmntok_t *tok, char *ack)
{
	bind = 1;

	DBGCN(BIND);

    DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC("6:::");
	client->print("6:::");

	DBGC(ack);
	client->print(ack);

	printByByte("+[{\"result\":\"ok\"}]");
  
	DBGCN((char)255);
	client->print((char)255);
}

void SkynetClient::processMessage(char *data, jsmntok_t *tok)
{
	//just give them the args
	int index = tok[5].end;
	data[index]=0;

	DBGCN(data + tok[5].start);

	if (messageDelegate != NULL) {
		messageDelegate(data + tok[5].start);
	}
}

void SkynetClient::processSkynet(char *data, char *ack)
{
	jsmn_parser p;
	jsmntok_t tok[MAX_PARSE_OBJECTS];

	jsmn_init(&p);

	int r = jsmn_parse(&p, data, tok, MAX_PARSE_OBJECTS);
	if (r != 0){
	    DBGCN(F("parse failed"));
		DBGCN(r);
		return;
	}

    if (TOKEN_STRING(data, tok[2], IDENTIFY )) 
    {
		DBGCN(IDENTIFY);
		processIdentify(data, tok);
    } 
    else if (TOKEN_STRING(data, tok[2], READY )) 
    {
		DBGCN(READY);
		processReady(data, tok);
    }
    else if (TOKEN_STRING(data, tok[2], NOTREADY )) 
    {
		DBGCN(NOTREADY);
		processNotReady(data, tok);
    }
    else if (TOKEN_STRING(data, tok[2], BIND )) 
    {
		DBGCN(BIND);
		processBind(data, tok, ack);
    }
    else if (TOKEN_STRING(data, tok[2], MESSAGE )) 
    {
		DBGCN(MESSAGE);
		processMessage(data, tok);
    }
    else
    {
		DBGC(F("Unknown:"));
    }
}

void SkynetClient::sendHandshake() {
	client->println(F("GET /socket.io/1/ HTTP/1.1"));
	client->print(F("Host: "));
	client->println(thehost);
	client->println(F("Origin: Arduino\r\n"));
}

bool SkynetClient::waitForInput(void) {
	unsigned long now = millis();
	while (!client->available() && ((millis() - now) < 30000UL)) {;}
	return client->available();
}

void SkynetClient::eatHeader(void) {
	while (client->available()) {	// consume the header
		readLineHTTP();
		if (strlen(databuffer) == 0) break;
	}
}

int SkynetClient::readHandshake() {

	if (!waitForInput()) return false;

	// check for happy "HTTP/1.1 200" response
	readLineHTTP();
	if (atoi(&databuffer[8]) != 200) {
		while (client->available()) readLineHTTP();
		client->stop();
		return 0;
	}
	eatHeader();
	readLineHTTP();	// read first line of response
	readLineHTTP();	// read sid : transport : timeout
		
	char sid[SID_LEN];
	char *iptr = databuffer;
	char *optr = sid;
	while (*iptr && (*iptr != ':') && (optr < &sid[SID_LEN-2])) *optr++ = *iptr++;
	*optr = 0;

	DBGC(F("Connected. SID="));
	DBGCN(sid);	// sid:transport:timeout 

	while (client->available()) readLineHTTP();

	client->print(F("GET /socket.io/1/websocket/"));
	client->print(sid);
	client->println(F(" HTTP/1.1"));
	client->print(F("Host: "));
	client->println(thehost);
	client->println(F("Origin: ArduinoSkynetClient"));
	client->println(F("Upgrade: WebSocket"));	// must be camelcase ?!
	client->println(F("Connection: Upgrade\r\n"));

	if (!waitForInput()) return 0;

	readLineHTTP();
	if (atoi(&databuffer[8]) != 101) {
		while (client->available()) readLineHTTP();
		client->stop();
		return false;
	}
	eatHeader();
	return 1;
}

int SkynetClient::readLineHTTP() {
	int numBytes = 0;
	char *dataptr = databuffer;
	DBGC(F("ReadlineHTTP: "));
	while (client->available() && (dataptr < &databuffer[SOCKET_RX_BUFFER_SIZE-3])) {
		char c = client->read();
		if (c == 0){
			;
		}else if (c == -1){
			;
		}else if (c == '\r') {
			;
		}else if (c == '\n') 
			break;
		else {
			DBGC(c);
			*dataptr++ = c;
			numBytes++;
		}
	}
	DBGCN();
	*dataptr = 0;
	return numBytes;
}

int SkynetClient::readLineSocket() {
	int numBytes = 0;
	char *dataptr = databuffer;
	DBGC(F("ReadlineSocket: "));
	//-1 for 0 index
	//-1 for space to add a char
	//-1 to fit a null char so
	while (client->available() && (dataptr < &databuffer[SOCKET_RX_BUFFER_SIZE-3])) {
		char c = client->read();
		if (c == 0){
			;
		}
		else if (c == -1){
			break;
		}
		else {
			DBGC(c);
			*dataptr++ = c;
			numBytes++;
		}
	}
	*dataptr = 0;
	DBGCN();
	return numBytes;
}

//wifi client->print has a buffer that so far we've been unable to locate
//under 154 (our identify size) for sure.. so sending char by char for now
void SkynetClient::printByByte(const char *data, size_t size) {
	if(data != NULL && data[0] != '\0')
	{
		int i = 0;
		while ( i < size)
		{
		    DBGC(data[i]);
			client->print(data[i++]);
		}
	}
}

//wifi client->print has a buffer that so far we've been unable to locate
//under 154 (our identify size) for sure.. so sending char by char for now
void SkynetClient::printByByte(const char *data) {
	if(data != NULL)
	{
		int i = 0;
		while ( data[i] != '\0' )
		{
		    DBGC(data[i]);
			client->print(data[i++]);
		}
	}
}

void SkynetClient::printToken(const char *js, jsmntok_t t) 
{
	int i = 0;
	for(i = t.start; i < t.end; i++) {
	    DBGC(js[i]);
		client->print(js[i]);
	 }
}

size_t SkynetClient::write(const uint8_t *buf, size_t size) {
    DBGC(F("Sending2: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(MSG);	
	client->print(MSG);
	
	//b64::send(buf, size, client);

    DBGCN((char)255);
	client->print((char)255);

	return size;
}

//place write data into a buffer to be sent on next flush or monitor
size_t SkynetClient::write(uint8_t c)
{
	if(bind){
		DBGC(F("Storing: "));

	    DBGCN((char)c);

	    txbuf.push(c);

		return 1;
	}
	else{
		DBGC(F("Not bound, NOT Storing: "));
	    DBGCN((char)c);

		return 0;
	}
}

void SkynetClient::flush()
{
	if(txbuf.available()){
		DBGC(F("Sending: "));
	
	    DBGC((char)0);
		client->print((char)0);
	
	    DBGC(MSG);	
		client->print(MSG);

		b64::send(txbuf, *client);
		
		DBGCN((char)255);
		client->print((char)255);
	}
}

int SkynetClient::available() {
  return rxbuf.available();
}

int SkynetClient::read() {
    // if the head isn't ahead of the tail, we don't have any characters
    if (rxbuf.available()) 
    {
    	return rxbuf.pop();
    } else {
    	return -1;
    }
}

// //TODO	
// int SkynetClient::read(uint8_t *buf, size_t size) {
// }

int SkynetClient::peek() 
{
    if (rxbuf.available()) 
    {
    	return rxbuf.peek();
    } else {
    	return -1;
    }
}

// the next function allows us to use the client returned by
// SkynetClient::available() as the condition in an if-statement.

SkynetClient::operator bool() {
  return true;
}

void SkynetClient::setMessageDelegate(MessageDelegate newMessageDelegate) {
	  messageDelegate = newMessageDelegate;
}

void SkynetClient::eeprom_write_bytes(int address, char *buf, int bufSize){
  for(int i = 0; i<bufSize; i++){
    EEPROM.write(address+i, buf[i]);
  }
}

void SkynetClient::eeprom_read_bytes(int address, char *buf, int bufSize){
  for(int i = 0; i<bufSize; i++){
    buf[i] = EEPROM.read(address+i);
  }
}

void SkynetClient::getToken(char *token){
	eeprom_read_bytes(TOKENADDRESS, token, TOKENSIZE);
	token[TOKENSIZE-1]='\0'; //in case courrupted or not defined
}

void SkynetClient::getUuid(char *uuid){
	eeprom_read_bytes(UUIDADDRESS, uuid, UUIDSIZE);
	uuid[UUIDSIZE-1]='\0'; //in case courrupted or not defined
}

void SkynetClient::sendMessage(const char *device, char const *object)
{
	DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);

	printByByte("{\"name\":\"message\",\"args\":[{\"devices\":\"");
	printByByte(device);
	printByByte("\",\"payload\":\"");
	printByByte(object);
	printByByte("\"}]}");

	DBGCN((char)255);
	client->print((char)255);
}