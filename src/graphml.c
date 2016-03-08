#include "graphml.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <libxml/parser.h>

#include "log.h"

typedef struct {
	size_t len;
	xmlChar* data;
} xmlCharBuffer;

const size_t DefaultXmlBufferLen = 255; // Arbitrary default; very generous for GraphML identifiers

static void initXmlCharBuffer(xmlCharBuffer* buffer) {
	buffer->len = DefaultXmlBufferLen;
	buffer->data = malloc(buffer->len);
}

static void freeXmlCharBuffer(xmlCharBuffer* buffer) {
	free(buffer->data);
}

// Copy a libxml string quickly, reallocating the target buffer if needed.
// We do this to prevent allocating memory for every element in the graph.
static void copyXmlStr(xmlCharBuffer* dst, const xmlChar* src) {
	// xmlChars represent bytes of UTF-8 code points. UTF-8 never contains a NUL
	// except for the final terminator, so we can treat these like C-strings for
	// the purpose of copying.
	size_t i = 0;
	bool overflow = false;
	while (true) {
		if (!overflow) {
			if (i >= dst->len) overflow = true;
			else dst->data[i] = src[i];
		}
		if (src[i] == 0) break;
		++i;
	}
	if (overflow) {
		dst->len = i * 2;
		dst->data = realloc(dst->data, dst->len);
		copyXmlStr(dst, src);
	}
}

typedef enum {
	GpUnknown,	// Inside an unknown element
	GpInitial,	// Looking for initial graphml element
	GpTopLevel,	// Waiting for keys or the graph
	GpGraph,	// Inside the graph element
	GpNode,		// Inside a node element
	GpEdge,		// Inside an edge element
	GpData,		// Inside a data element
} GraphParserMode;

// Parser state for GraphML files
typedef struct {
	GraphParserMode mode;

	// Value used in nodes' type attribute to indicate that they are a client
	const char* clientType;

	// GpUnknown state
	unsigned int unknownDepth;		// The depth since the first unknown element
	GraphParserMode unknownMode;	// The previous mode

	// Graph parameters
	bool defaultUndirected;	// True if edges are undirected by default

	// Attribute identifiers
	struct {
		xmlChar* typeId;
		xmlChar* packetLossId;
		xmlChar* bandwidthUpId;
		xmlChar* bandwidthDownId;
	} nodeAttribs;
	struct {
		xmlChar* latencyId;
		xmlChar* packetLossId;
		xmlChar* jitterId;
		xmlChar* queueLenId;
	} edgeAttribs;

	// Attribute values
	xmlCharBuffer dataKey;		// Key for the data being parsed
	xmlChar* dataValue;			// Buffer for the data value
	size_t dataValueLen;		// Size of the data value buffer contents
	size_t dataValueCap;		// Capacity of the data value buffer
	GraphParserMode dataMode;	// Mode to return to after parsing the data

	// Node and link objects used to pass to the callers
	GraphNode node;
	xmlCharBuffer nodeId;
	GraphLink link;
	xmlCharBuffer linkSourceId;
	xmlCharBuffer linkTargetId;

	// Error handling state
	bool partialError;	// True when libxml has sent an error message without a newline
	bool dead;			// True when a fatal error has been encountered

	// Data necessary for calling back to the client
	void* userData;
	NewNodeFunc newNodeFunc;
	NewLinkFunc newLinkFunc;
} GraphParserState;

// Prepend internal libxml errors
static void showXmlError(void* ctx, const char* msg, ...) {
	va_list args;
	va_start(args, msg);

	// Render the message into memory so that we can examine its contents
	const int defaultBufferSize = 255;
	char* errBuffer = malloc(defaultBufferSize);
	int neededChars = vsnprintf(errBuffer, defaultBufferSize, msg, args);
	if (neededChars >= defaultBufferSize) {
		errBuffer = realloc(errBuffer, neededChars+1);
		neededChars = vsnprintf(errBuffer, defaultBufferSize, msg, args);
	}

	// Redirect the error to our logging system
	if (neededChars < 0) {
		lprintf(LogError, "Could not display libxml error (code: %d)\n", neededChars);
	} else {
		GraphParserState* state = (GraphParserState*)ctx;
		if (!state->partialError) {
			lprintHead(LogError);
			lprintDirectf(LogError, "libxml error while parsing GraphML file: ");
		}
		lprintDirectf(LogError, "%s", errBuffer);

		// The error is considered partial if it is non-empty and doesn't end with a newline
		char* p;
		for (p = errBuffer; *p; ++p);
		state->partialError = (errBuffer[0] != '\0' && p[-1] != '\n');
	}

	free(errBuffer);
	va_end(args);
}

// Display a parsing error and terminate the parsing
static void graphFatalError(GraphParserState* state, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	lprintHead(LogError);
	lprintDirectf(LogError, "GraphML parse error: ");
	lvprintDirectf(LogError, fmt, args);
	va_end(args);
	state->dead = true;
}

static void initGraphParserState(GraphParserState* state, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType) {
	state->clientType = clientType;
	state->newNodeFunc = newNode;
	state->newLinkFunc = newLink;
	state->userData = userData;
	state->mode = GpInitial;
	state->defaultUndirected = false;
	initXmlCharBuffer(&state->dataKey);
	state->dataValueCap = DefaultXmlBufferLen;
	state->dataValue = malloc(state->dataValueCap);
	state->dataValueLen = 0;
	initXmlCharBuffer(&state->nodeId);
	initXmlCharBuffer(&state->linkSourceId);
	initXmlCharBuffer(&state->linkTargetId);
	state->partialError = false;
	state->dead = false;
	memset(&state->nodeAttribs, 0, sizeof(state->nodeAttribs));
	memset(&state->edgeAttribs, 0, sizeof(state->edgeAttribs));
	xmlSetGenericErrorFunc(state, &showXmlError);
}

static void cleanupGraphParserState(GraphParserState* state) {
	freeXmlCharBuffer(&state->dataKey);
	if (state->dataValue) free(state->dataValue);
	freeXmlCharBuffer(&state->nodeId);
	freeXmlCharBuffer(&state->linkSourceId);
	freeXmlCharBuffer(&state->linkTargetId);

	// Free attribute identifiers. Written this way to avoid another place with explicit names.
	#define FREE_ATTRIBS(objType) \
		for (size_t i = 0; i < sizeof(state->objType##Attribs); i += sizeof(xmlChar*)) { \
			xmlChar** p = (xmlChar**)(((char*)&state->objType##Attribs) + i); \
			if (*p) free(*p); \
		}
	FREE_ATTRIBS(node);
	FREE_ATTRIBS(edge);
}

static void graphStartElement(void* ctx, const xmlChar* name, const xmlChar** atts) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;
	bool unknown = false;

	switch (state->mode) {
	case GpUnknown:
		++state->unknownDepth;
		break;

	case GpInitial:
		if (!xmlStrEqual(name, (const xmlChar*)"graphml")) {
			graphFatalError(state, "The topology file is not a GraphML file.\n");
			break;
		}
		for (const xmlChar** att = atts; *att; att += 2) {
			if (xmlStrEqual(att[0], (const xmlChar*)"xmlns")) {
				if (!xmlStrEqual(att[1], (const xmlChar*)"http://graphml.graphdrawing.org/xmlns")) {
					graphFatalError(state, "The topology file used an unknown GraphML namespace.\n");
				}
				break;
			}
		}
		state->mode = GpTopLevel;
		break;

	case GpTopLevel:
		if (xmlStrEqual(name, (const xmlChar*)"key")) {
			const xmlChar* name = NULL;
			const xmlChar* id = NULL;
			const xmlChar* type = NULL;
			const xmlChar* keyFor = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"attr.name")) name = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"id")) id = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"attr.type")) type = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"for")) keyFor = att[1];
			}

			if (name && id && type && keyFor) {
				// Convenience macro used to record attribute identifiers.
				// We store it explicitly rather than using a hash map for performance reasons.
				#define CHECK_SET_ATTR(key, acceptInt, acceptFloat, acceptStr, objType, attr) \
					if (xmlStrEqual(name, (const xmlChar*)key)) { \
						bool correctType = false; \
						if (xmlStrEqual(type, (const xmlChar*)"int") || xmlStrEqual(type, (const xmlChar*)"long")) correctType = (acceptInt); \
						else if (xmlStrEqual(type, (const xmlChar*)"float") || xmlStrEqual(type, (const xmlChar*)"double")) correctType = (acceptFloat); \
						else if (xmlStrEqual(type, (const xmlChar*)"string")) correctType = (acceptStr); \
						if (!correctType) { \
							graphFatalError(state, "The key '%s' in the topology file had unexpected type '%s'.\n", key, type); \
						} else state->objType##Attribs.attr##Id = xmlStrdup(id); \
					}

				// Record the attribute if it is one of the known ones
				if (xmlStrEqual(keyFor, (const xmlChar*)"node")) {
					CHECK_SET_ATTR("type", false, false, true, node, type)
					else CHECK_SET_ATTR("packetloss", true, true, false, node, packetLoss)
					else CHECK_SET_ATTR("bandwidthup", true, true, false, node, bandwidthUp)
					else CHECK_SET_ATTR("bandwidthdown", true, true, false, node, bandwidthDown)
				} else if (xmlStrEqual(keyFor, (const xmlChar*)"edge")) {
					CHECK_SET_ATTR("latency", true, true, false, edge, latency)
					else CHECK_SET_ATTR("packetloss", true, true, false, edge, packetLoss)
					else CHECK_SET_ATTR("jitter", true, true, false, edge, jitter)
					else CHECK_SET_ATTR("queue_len", true, false, false, edge, queueLen)
				}
			}
			unknown = true;
		} else if (xmlStrEqual(name, (const xmlChar*)"graph")) {
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"edgedefault")) {
					state->defaultUndirected = xmlStrEqual(att[1], (const xmlChar*)"undirected");
					break;
				}
			}
			state->mode = GpGraph;
		} else unknown = true;
		break;

	case GpGraph:
		if (xmlStrEqual(name, (const xmlChar*)"node")) {
			const xmlChar* id = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"id")) {
					id = att[1];
				}
			}
			if (!id) graphFatalError(state, "Topology contained a node without an identifier.\n");
			else {
				copyXmlStr(&state->nodeId, id);
				state->node.Id = (const char*)state->nodeId.data;
				state->node.Client = (state->clientType != NULL ? false : true);
				state->node.PacketLoss = 0.0;
				state->node.BandwidthUp = 0;
				state->node.BandwidthDown = 0;
				state->mode = GpNode;
			}
		} else if (xmlStrEqual(name, (const xmlChar*)"edge")) {
			bool undirected = state->defaultUndirected;
			const xmlChar* source = NULL;
			const xmlChar* target = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"directed")) {
					if (xmlStrEqual(att[1], (const xmlChar*)"false")) {
						undirected = true;
					} else {
						undirected = false;
					}
				} else if (xmlStrEqual(att[0], (const xmlChar*)"source")) {
					source = att[1];
				} else if (xmlStrEqual(att[0], (const xmlChar*)"target")) {
					target = att[1];
				}
			}
			if (!source) graphFatalError(state, "Topology contained an edge that did not specify a source node.\n");
			else if (!target) graphFatalError(state, "Topology contained an edge that did not specify a target node.\n");
			else if (!undirected) graphFatalError(state, "Topology contained a directed edge from '%s' to '%s'. Only undirected edges are supported.\n", source, target);
			else {
				copyXmlStr(&state->linkSourceId, source);
				copyXmlStr(&state->linkTargetId, target);
				state->link.SourceId = (const char*)state->linkSourceId.data;
				state->link.TargetId = (const char*)state->linkTargetId.data;
				state->link.Latency = 0.0;
				state->link.PacketLoss = 0.0;
				state->link.Jitter = 0.0;
				state->link.QueueLen = 0;
				state->mode = GpEdge;
			}
		} else unknown = true;
		break;

	case GpNode:
	case GpEdge:
		if (xmlStrEqual(name, (const xmlChar*)"data")) {
			bool foundKey = false;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"key")) {
					foundKey = true;
					copyXmlStr(&state->dataKey, att[1]);
					break;
				}
			}
			if (!foundKey) graphFatalError(state, "Topology contains a data attributed with no key.\n");
			state->dataValueLen = 0;
			state->dataMode = state->mode;
			state->mode = GpData;
		} else unknown = true;
		break;

	case GpData:
		unknown = true;
		break;

	default: graphFatalError(state, "BUG: Unknown GraphML parser state %d for startElement!\n", state->mode);
	}

	if (unknown) {
		state->unknownMode = state->mode;
		state->mode = GpUnknown;
		state->unknownDepth = 0;
	}
}

static void graphEndElement(void* ctx, const xmlChar* name) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;

	switch (state->mode) {
	case GpUnknown:
		if (state->unknownDepth == 0) {
			state->mode = state->unknownMode;
		} else {
			--state->unknownDepth;
		}
		break;

	case GpData: {
		// Add NUL terminator if characters were read
		const xmlChar* value = (const xmlChar*)"";
		if (state->dataValue) {
			state->dataValue[state->dataValueLen] = 0;
			value = state->dataValue;
		}

		switch (state->dataMode) {
		case GpNode:
			if (state->nodeAttribs.typeId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.typeId)) {
				state->node.Client = (xmlStrEqual(value, (const xmlChar*)"client"));
			} else if (state->nodeAttribs.packetLossId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.packetLossId)) {
				state->node.PacketLoss = strtod((const char*)value, NULL);
			} else if (state->nodeAttribs.bandwidthUpId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.bandwidthUpId)) {
				state->node.BandwidthUp = strtod((const char*)value, NULL);
			} else if (state->nodeAttribs.bandwidthDownId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.bandwidthDownId)) {
				state->node.BandwidthDown = strtod((const char*)value, NULL);
			}
			break;

		case GpEdge:
			if (state->edgeAttribs.latencyId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.latencyId)) {
				state->link.Latency = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.packetLossId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.packetLossId)) {
				state->link.PacketLoss = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.jitterId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.jitterId)) {
				state->link.Jitter = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.queueLenId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.queueLenId)) {
				state->link.QueueLen = (uint32_t)strtoul((const char*)value, NULL, 10);
			}
			break;

		default: graphFatalError(state, "BUG: Unknown GraphML data state %d when parsing key!\n", state->dataMode);
		}

		state->mode = state->dataMode;
		break;
	}

	case GpNode:
		state->newNodeFunc(&state->node, state->userData);
		state->mode = GpGraph;
		break;

	case GpEdge:
		state->newLinkFunc(&state->link, state->userData);
		state->mode = GpGraph;
		break;

	case GpGraph:
		state->mode = GpTopLevel;
		break;

	case GpTopLevel:
		state->mode = GpUnknown;
		state->unknownDepth = 0;
		state->unknownMode = GpUnknown;
		break;

	default: graphFatalError(state, "BUG: Unknown GraphML parser state %d for endElement!\n", state->mode);
	}
}

static void graphCharacters(void* ctx, const xmlChar* ch, int len) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;

	if (state->mode == GpData) {
		// Append the new UTF-8 characters to the data buffer
		size_t newSize = state->dataValueLen + (size_t)len;
		if (newSize > state->dataValueCap) {
			state->dataValueCap = newSize * 2;
			state->dataValue = realloc(state->dataValue, state->dataValueCap+1);
		}
		memcpy(&state->dataValue[state->dataValueLen], ch, len);
		state->dataValueLen = newSize;
	}
}

// Callbacks for parsing GraphML files with libxml SAX interface
static xmlSAXHandler graphHandlers = {
	startElement: &graphStartElement,
	endElement: &graphEndElement,
	characters: &graphCharacters,

	warning: &showXmlError,
	error: &showXmlError,
	fatalError: &showXmlError,
};

int parseGraph(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType) {
#ifdef LIBXML_PUSH_ENABLED
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType);
	xmlParserCtxtPtr xmlContext = NULL;
	int xmlError = 0;

	// Read the input in chunks
	const size_t chunkSize = 1024 * 8;
	size_t read;
	char* buffer = malloc(chunkSize);
	do {
		read = fread(buffer, 1, chunkSize, input);
		if (read > 0) {
			if (!xmlContext) {
				// Initialize the context using the first chunk to detect the encoding
				xmlContext = xmlCreatePushParserCtxt(&graphHandlers, &state, buffer, read, NULL);
				if (!xmlContext) {
					xmlError = -1;
				} else {
					xmlSetGenericErrorFunc(xmlContext, &showXmlError);
				}
			} else {
				xmlError = xmlParseChunk(xmlContext, buffer, read, 0);
			}
		}
	} while (read == chunkSize && !xmlError);

	// If there were no problems, terminate the reading
	if (xmlContext && !xmlError) {
		xmlError = xmlParseChunk(xmlContext, NULL, 0, 1);
	}
	free(buffer);
	cleanupGraphParserState(&state);

	// Handle errors
	int readError = ferror(input);
	int error = readError ? readError : xmlError;
	if (error) {
		lprintf(LogError, "Failed to parse the GraphML file (error: %d). The document may be malformed.", error);
	}
	return error;
#else
	lprintln(LogError, "Reading GraphML content in this mode is not supported because libxml was compiled without push parser support");
	return -1;
#endif
}

int parseGraphFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType) {
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType);
	int result = xmlSAXUserParseFile(&graphHandlers, &state, filename);
	cleanupGraphParserState(&state);
	return result;
}

int parseGraphMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType) {
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType);
	int result = xmlSAXUserParseMemory(&graphHandlers, &state, buffer, size);
	cleanupGraphParserState(&state);
	return result;
}
