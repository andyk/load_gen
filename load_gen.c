#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <getopt.h>

//TODO: Print how well we match the requested rate

// Command-line options
static char * host = NULL;
static FILE * tracefile = NULL;
static FILE * urlfile = NULL;
static char * loadBalHostsFileName = NULL; //file holding urls to send load to
static FILE * loadBalHostsFile = NULL;
static char currentHost[301] = ""; //used by each request when in round-rob mode
static FILE * logfile = NULL;
static char * geturl = NULL;
static double rate = -1;
static int num_req = -1;
static int timeout = 10;

// How long to wait before running the trace
static const unsigned int load_time_s = 3;

static struct event_base *base;

static void write_debug(int severity, const char *msg)
{
    const char *s;
    switch (severity) {
        case _EVENT_LOG_DEBUG: s = "debug"; break;
        case _EVENT_LOG_MSG:   s = "msg";   break;
        case _EVENT_LOG_WARN:  s = "warn";  break;
        case _EVENT_LOG_ERR:   s = "error"; break;
        default:               s = "?";     break; /* never reached */
    }
    if (severity != _EVENT_LOG_DEBUG)
      fprintf(stderr, "[%s] %s\n", s, msg);
}

struct request {
  int id;
  struct timeval start_time;  //When the request started
  struct timeval delta;  //Delta until next req
  struct event *timer;
  struct evhttp_request *evreq;
  struct evhttp_connection *evcon;
  char url[301];
};

#define US_TO_MS(usec) ((int)((double) usec / 1E3))
static void log_start(int id, struct timeval *now, struct timeval *our_delta, struct timeval *ideal_delta)
{
  struct timeval diff;
  if (evutil_timercmp(our_delta, ideal_delta, <)) {
    // Too fast
    evutil_timersub(ideal_delta, our_delta, &diff);
    diff.tv_sec *= -1;
  } else {
    // Too slow
    evutil_timersub(our_delta, ideal_delta, &diff);
  }

  // Note: delay could be negative for start time
  fprintf(logfile, "%lu.%03u start %d %ld.%03u\n",
    now->tv_sec, US_TO_MS(now->tv_usec), id,
    diff.tv_sec, US_TO_MS(diff.tv_usec));
}

static void log_conn(int id, struct timeval *now, struct timeval *delay)
{
  // Note: delay should always be positive
  fprintf(logfile, "%lu.%03u conn %d %lu.%03u\n",
    now->tv_sec, US_TO_MS(now->tv_usec), id,
    delay->tv_sec, US_TO_MS(delay->tv_usec));
}

static void log_resp(int id, struct timeval *start, struct timeval *end, int resp_code, short error)
{
  char *serror;
  struct timeval resp;
  evutil_timersub(end, start, &resp);

  if (error == 0)
    serror = "ok";
  else if (error & BEV_EVENT_ERROR)
    serror = "other";
  else if (error & BEV_EVENT_TIMEOUT)
    serror = "timeout";
  else if (error & BEV_EVENT_CONNECTED)
    serror = "connect";
  else {
    fprintf(stderr, "Unknown error code 0x%x\n", error);
    serror = "UNKNOWN";
  }

  fprintf(logfile, "%lu.%03u end %d %d %s %lu.%03u\n",
    end->tv_sec, US_TO_MS(end->tv_usec), id, resp_code, serror,
    resp.tv_sec, US_TO_MS(resp.tv_usec));

}

void http_request_done(struct evhttp_request * req, void * arg)
{
  struct timeval end; 
  struct request *myreq = (struct request *) arg;
  assert(myreq != NULL);

  gettimeofday(&end, NULL);
  log_resp(myreq->id, &myreq->start_time, &end, req->response_code, req->error);

  //Debug:
  // char * buf = (char *) malloc(evbuffer_get_length(req->input_buffer));
  // evbuffer_remove(req->input_buffer, buf, evbuffer_get_length(req->input_buffer));
  // printf("%s\n", buf);

  evhttp_connection_free(myreq->evcon);  
  free(myreq);
}

void http_conn(struct evhttp_connection * evcon, void *arg)
{
  struct timeval now, delay;
  struct request *req = (struct request *) arg;
  assert(req != NULL);

  // Compute delay from start
  gettimeofday(&now, NULL);
  evutil_timersub(&now, &req->start_time, &delay);
  log_conn(req->id, &now, &delay);
}

#define ROUND(a) ((int) (a / 10.0) * 10)
#define PRINT_TIME(t) printf("%ld.%03d\n", (t).tv_sec, US_TO_MS((t).tv_usec))

static int get_next_url(FILE *file, char *url)
{
  int rc;
  if ((rc = fscanf(file, "%300s\n", url)) == 1)
    return 1;

  if (rc == EOF) {
    rewind(file);
    return (fscanf(file, "%300s\n", url) == 1);
  }

  return 0;
}

static int get_next_load_balance_host(char *url)
{
  int rc;
  if ((rc = fscanf(loadBalHostsFile, "%300s\n", url)) == 1)
    return 1;

  if (rc == EOF) {
    fclose(loadBalHostsFile);
    loadBalHostsFile = fopen(loadBalHostsFileName, "r");
    return (fscanf(loadBalHostsFile, "%300s\n", url) == 1);
  }

  return 0;
}
    

void send_req(evutil_socket_t fd, short what, void *arg)
{
  static struct timeval last_time = {0, 0};
  struct timeval delta;
  static int progress = 0;
  double percent;
  struct request *req = (struct request *) arg;
  assert(req != NULL);
  assert(what&EV_TIMEOUT);  //Timer expired

  gettimeofday(&req->start_time, NULL);
  if (req->id == 0) {
    fprintf(stderr, "Start sending\n");
    last_time = req->start_time;
  }
  evutil_timersub(&req->start_time, &last_time, &delta);
  last_time = req->start_time;
 
  percent = (double) (req->id + 1) / num_req * 100; 
  if (ROUND(percent) != progress) {
    progress = ROUND(percent); 
    fprintf(stderr, "Sending %d%% done\n", progress);
  } 

  log_start(req->id, &req->start_time, &delta, &req->delta);

  // set up next host url from load balancer file
  if (loadBalHostsFile != NULL) {
    //update currentHost to be next in round robin list
    get_next_load_balance_host(currentHost);
    fprintf(stderr, "Next host url from load balancer file (%s) is %s.\n", loadBalHostsFileName, currentHost);
  } else {
    strcpy(currentHost,host);
    fprintf(stderr, "Using host %s from --host flag.\n", currentHost);
  }

  fprintf(stderr, "Setting up connection to host %s\n", currentHost);
  req->evcon = evhttp_connection_base_new(base, NULL, currentHost, 80);
  if (req->evcon == NULL) {
    fprintf(stderr, "evhttp_connection failed\n");
    exit(1);
  }
  
  evhttp_connection_set_timeout(req->evcon, timeout);
  evhttp_connection_set_conncb(req->evcon, http_conn, req);
  req->evreq = evhttp_request_new(http_request_done, req);
  evhttp_add_header(req->evreq->output_headers, "Host", "trace-play");

  if (evhttp_make_request(req->evcon, req->evreq, EVHTTP_REQ_GET, req->url) == -1) {
    fprintf(stderr, "Request failed\n");
    exit(1);
  }

  event_free(req->timer);
  req->timer = NULL;
}  

static void load_trace_file(FILE *file)
{
  int first_line = 1;
  struct timeval last_time, first_time;

  num_req = 0;
  while (!feof(file)) {
    struct request *req = (struct request *) malloc(sizeof(struct request));
    struct timeval read_time, time;
    
    if (fscanf(file, "%ld.%3ld %200s\n", &read_time.tv_sec, &read_time.tv_usec, req->url) != 3) {
      fprintf(stderr, "Parse error on line %d\n", num_req);
      exit(1);
    }
    read_time.tv_usec *= 1000;


    if (first_line) first_time = read_time;
    evutil_timersub(&read_time, &first_time, &time);
    time.tv_sec += load_time_s;

    if (first_line) {
      last_time = time;
      first_line = 0;
    }
    evutil_timersub(&time, &last_time, &req->delta);
    last_time = time;

    req->id = num_req++;

    req->timer = evtimer_new(base, send_req, req);
    evtimer_add(req->timer, &time);
  }
}

static void load_url_file(FILE *file, char *url, double rate, int num_conn)
{
  int i;
  double period_d = 1 / rate;
  struct timeval period;
  struct timeval last_time, time;

  period.tv_sec = (long) period_d;
  period.tv_usec = (long) ((period_d - period.tv_sec) * 1E6);

  time.tv_sec = load_time_s;
  time.tv_usec = 0;
  last_time = time;

  // Exactly one should be set
  assert((file != NULL) || (url != NULL));
  assert((file == NULL) || (url == NULL));

  for (i = 0; i < num_conn; i++) {
    struct request *req = (struct request *) malloc(sizeof(struct request));
   
    if (url != NULL) { 
      strncpy(req->url, url, 300);
    } else {
      if (get_next_url(file, req->url) == 0) {
        fprintf(stderr, "Error reading url.\n");
        exit(1);
      }
    }
    req->id = i; 

    req->timer = evtimer_new(base, send_req, req);
    evtimer_add(req->timer, &time);
    
    evutil_timersub(&time, &last_time, &req->delta);
    last_time = time;
    evutil_timeradd(&time, &period, &time);
  }
}

static void usage()
{
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, " load_gen \n");
  fprintf(stderr, "   (--host ip_address or --loadbalfile load_bal_host_file)\n");
  fprintf(stderr, "   [--trace trace_file]\n");
  fprintf(stderr, "   ( [--wlog url_file] or [--url] ) [--rate] [--num-req]\n");
  fprintf(stderr, "   [--timeout socket_timeout]\n");
  fprintf(stderr, "   [--output out_file]\n");
  exit(1);
}

#define error(s) do { printf(s); usage(); } while(0)
static void options(int argc, char **argv)
{
  int ct = 0;
  // name=long opt name
  // has_arg= (no_argument, required_argument, optional_argument
  // flag=how results are returned for a long opt
  // val=return val or to load into variable pointed to by flag
  static struct option long_options[] = {
      {"host",        required_argument, 0, 'h'},
      {"trace",       required_argument, 0, 't'},
      {"url",         required_argument, 0, 'u'},
      {"wlog",        required_argument, 0, 'w'},
      {"loadbalfile", required_argument, 0, 'b'},
      {"rate",        required_argument, 0, 'r'},
      {"num-req",     required_argument, 0, 'n'},
      {"timeout",     required_argument, 0, 'i'},
      {"output",      required_argument, 0, 'o'},
      {"help",        no_argument      , 0, 'l'},
      {0, 0, 0, 0}
  };

  while (1) {
    int option_index = 0;
    int c = getopt_long (argc, argv, "h:t:u:w:b:r:n:i:o:l", long_options, &option_index);
    
    if (c == -1)
      break;

    switch (c) {
      case 'h':
        host = malloc(strlen(optarg));
        strcpy(host, optarg);
        break;
      case 't':
        tracefile = fopen(optarg, "r");
        if (tracefile == NULL) {
          fprintf(stderr, "Error opening file %s\n", optarg);
          exit(1);
        }
        break;
      case 'w':
        urlfile = fopen(optarg, "r");
        if (urlfile == NULL) {
          fprintf(stderr, "Error opening file %s\n", optarg);
          exit(1);
        }
        break;
      case 'b':
        loadBalHostsFileName = malloc(strlen(optarg));
        strcpy(loadBalHostsFileName, optarg);
        loadBalHostsFile = fopen(optarg, "r");
        if (loadBalHostsFile == NULL) {
          fprintf(stderr, "Error opening file %s\n", optarg);
          exit(1);
        }
        break;
      case 'u':
        geturl = malloc(strlen(optarg));
        strcpy(geturl, optarg);
        break;
      case 'r':
        rate = atof(optarg);
        if (rate <= 0) {
          fprintf(stderr, "Invalid rate %f\n", rate);
          exit(1);
        }
        break;
      case 'n':
        num_req = atoi(optarg);
        if (num_req <= 0) {
          fprintf(stderr, "Invalid number of requests %d\n", num_req);
          exit(1);
        }
        break;
      case 'i':
        timeout = atoi(optarg);
        if (timeout < 0) {
          fprintf(stderr, "Invalid timeout %d\n", timeout);
          exit(1);
        }
        break;
      case 'o':
        logfile = fopen(optarg, "w");
        if (logfile == NULL) {
          fprintf(stderr, "Error opening file %s\n", optarg);
          exit(1);
        }
        break;
      case 'l':
      default:
        usage();
    }
  }

  // Check args
  if (loadBalHostsFile != NULL) ct++;
  if (host != NULL) ct++;
  if (ct != 1)
    error("Please set exactly one of --host or --loadBalHostsFile\n");

  ct = 0;
  if (tracefile != NULL) ct++;
  if (urlfile != NULL) ct++;
  if (geturl != NULL) ct++;
  if (ct != 1)  
    error("Please set --trace or --wlog or --url\n");

  if (tracefile != NULL) {
    if (rate != -1 || num_req != -1)
      error("Rate and num-req do not apply to trace replay.\n");
  } else if (urlfile != NULL || geturl != NULL) {
    if (rate == -1 || num_req == -1)
      error("Rate and num-req parameters required for url mode.\n");
  }

  if (logfile == NULL)
    logfile = stdout;

}

void handle_sig (int sig)
{
  fprintf(stderr, "Stopping...\n");
  if (logfile)
    fflush(logfile);
  event_base_loopbreak(base); 
}

int main (int argc, char **argv)
{ 
  struct sigaction my_action;

  my_action.sa_handler = handle_sig;
  my_action.sa_flags = SA_RESTART;
  sigaction (SIGTERM, &my_action, NULL);
  sigaction (SIGINT, &my_action, NULL);

  options(argc, argv);

  event_set_log_callback(write_debug);
  base = event_base_new();

  fprintf(stderr, "Loading...\n");

  if (urlfile != NULL)
    load_url_file(urlfile, NULL, rate, num_req);
  else if (geturl != NULL)
    load_url_file(NULL, geturl, rate, num_req);
  else if (tracefile != NULL)
    load_trace_file(tracefile);
  else
    assert(0);
 
  if (tracefile) fclose(tracefile);
  if (urlfile) fclose(urlfile);

  // Run main event loop
  event_base_dispatch(base);

  // Print final stats

  event_base_free(base);
  if (logfile) fclose(logfile);

  return 0;
}
