/*
 * Copyright 2011 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: nforman@google.com (Naomi Forman)
//
// Implements the insert_ga_snippet filter, which inserts the
// Google Analytics tracking snippet into html pages.

#include "net/instaweb/rewriter/public/insert_ga_filter.h"

#include "base/logging.h"
#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/http/google_url.h"

namespace {

// Name for statistics variable.
const char kInsertedGaSnippets[] = "inserted_ga_snippets";

}  // namespace

namespace net_instaweb {

// Google Analytics snippet for setting experiment related variables.
extern const char kGAExperimentSnippet[] =
    "var _gaq = _gaq || [];"
    "%s"  // %s is the optional snippet to increase site speed tracking.
    "%s";  // %s is the snippet for experiments.

// Google Analytics async snippet along with the _trackPageView call.
extern const char kGAJsSnippet[] =
    "if (window.parent == window) {"
    "var _gaq = _gaq || [];"
    "_gaq.push(['_setAccount', '%s']);"  // %s is the GA account number.
    "_gaq.push(['_setDomainName', '%s']);"  // %s is the domain name
    "_gaq.push(['_setAllowLinker', true]);"
    "_gaq.push(['_trackPageview']);"
    "(function() {"
    "var ga = document.createElement('script'); ga.type = 'text/javascript';"
    "ga.async = true;"
    "ga.src = '%s.google-analytics.com/ga.js';"  // %s is the scheme and www/ssl
    "var s = document.getElementsByTagName('script')[0];"
    "s.parentNode.insertBefore(ga, s);"
    "})();"
    "}";

// Set the sample rate to 100%.
// TODO(nforman): Allow this to be configurable through RewriteOptions.
extern const char kGASpeedTracking[] =
    "_gaq.push(['_setSiteSpeedSampleRate', 100]);";

// The %u is for the variable slot (defaults to 1).
// The %s is for the Experiment spec string.
// This defaults to being a page-scoped variable.
const char kExperimentSnippetFmt[] =
    "_gaq.push(['_setCustomVar', %u, 'ExperimentState', '%s']);";

InsertGAFilter::InsertGAFilter(RewriteDriver* rewrite_driver)
    : CommonFilter(rewrite_driver),
      script_element_(NULL),
      added_analytics_js_(false),
      added_experiment_snippet_(false),
      ga_id_(rewrite_driver->options()->ga_id()),
      found_snippet_(false),
      increase_speed_tracking_(
          rewrite_driver->options()->increase_speed_tracking()) {
  Statistics* stats = driver()->statistics();
  inserted_ga_snippets_count_ = stats->GetVariable(kInsertedGaSnippets);
  DCHECK(!ga_id_.empty()) << "Enabled ga insertion, but did not provide ga id.";
}

void InsertGAFilter::InitStats(Statistics* stats) {
  stats->AddVariable(kInsertedGaSnippets);
}

InsertGAFilter::~InsertGAFilter() {}

void InsertGAFilter::StartDocumentImpl() {
  found_snippet_ = false;
  script_element_ = NULL;
  added_analytics_js_ = false;
  added_experiment_snippet_ = false;
  buffer_.clear();
  if (driver()->options()->running_experiment()) {
    driver()->message_handler()->Message(
        kInfo, "run_experiment: %s",
        driver()->options()->ToExperimentDebugString().c_str());
  }
}

// Add the experiment js snippet at the beginning of <head> and then
// start looking for ga snippet.
void InsertGAFilter::StartElementImpl(HtmlElement* element) {
  if (!added_experiment_snippet_) {
    if (element->keyword() == HtmlName::kHead) {
      added_experiment_snippet_ = true;
      // This will be empty if we're not running experiment.
      GoogleString experiment = ConstructExperimentSnippet();
      // Increase the percentage of traffic for which we track page load time.
      GoogleString speed_snippet = "";
      if (!experiment.empty() || increase_speed_tracking_) {
        speed_snippet = kGASpeedTracking;
      }
      GoogleString snippet_text = StringPrintf(
          kGAExperimentSnippet,
          speed_snippet.c_str(), experiment.c_str());
      AddScriptNode(element, snippet_text, true);
    }
  }
  if (!found_snippet_ && element->keyword() == HtmlName::kScript &&
      script_element_ == NULL) {
    script_element_ = element;
    buffer_.clear();
  }
}

// This may not be exact, but should be a pretty good guess.
// TODO(nforman): Find out if there is a canonical way of determining
// if a script is a GA snippet.
// TODO(anupama): If the existing GA snippet does not have a _trackPagview
// call, we may not get meaningful results. See if this case needs to be fixed.
bool InsertGAFilter::FoundSnippetInBuffer() const {
  return
      (buffer_.find(ga_id_) != GoogleString::npos &&
       buffer_.find("setAccount") != GoogleString::npos &&
       (buffer_.find(".google-analytics.com/ga.js") != GoogleString::npos ||
        buffer_.find(".google-analytics.com/urchin.js") != GoogleString::npos));
}

// Running an experiment: add in the information as the slot 1 custom variable.
// TODO(nforman): Change this to be a label on track_timings
// data when the track_timings api goes live (maybe).
GoogleString InsertGAFilter::ConstructExperimentSnippet() const {
  GoogleString experiment = "";
  if (driver()->options()->running_experiment()) {
    int experiment_state = driver()->options()->experiment_id();
    if (experiment_state != experiment::kExperimentNotSet &&
        experiment_state != experiment::kNoExperiment) {
      experiment = StringPrintf(kExperimentSnippetFmt,
          driver()->options()->experiment_ga_slot(),
          driver()->options()->ToExperimentString().c_str());
    }
  }
  return experiment;
}

void InsertGAFilter::AddScriptNode(HtmlElement* current_element,
                                   GoogleString text,
                                   bool insert_immediately_after_current) {
  HtmlElement* script_element = driver()->NewElement(current_element,
                                                     HtmlName::kScript);
  script_element->set_style(HtmlElement::EXPLICIT_CLOSE);
  driver()->AddAttribute(script_element, HtmlName::kType,
                         "text/javascript");
  HtmlNode* snippet =
      driver()->NewCharactersNode(script_element, text);
  if (insert_immediately_after_current) {
    driver()->InsertNodeAfterCurrent(script_element);
  } else {
    driver()->AppendChild(current_element, script_element);
  }
  driver()->AppendChild(script_element, snippet);
}

GoogleString InsertGAFilter::MakeFullExperimentSnippet() const {
  GoogleString experiment = ConstructExperimentSnippet();
  if (!experiment.empty()) {
    // Always increase speed tracking to 100% for experiments.
    StrAppend(&experiment, kGASpeedTracking);
  }
  return experiment;
}

// Handle the end of a body tag.
// If we've already inserted any GA snippet or if we found a GA
// snippet in the original page, don't do anything.
// If we haven't found anything, and haven't inserted anything yet,
// insert the GA js snippet.
// Caveat: Analytics js should ideally be placed in <head> for accurate
// collection of data (e.g. pageviews etc.). We place it at the end of the
// <body> tag so that we won't add duplicate analytics js code for any page.
// For pages which don't already have analytics js, this might result in some
// data being lost.
void InsertGAFilter::HandleEndBody(HtmlElement* body) {
  // There is a chance (e.g. if there are two body tags), that we have
  // already inserted the snippet.  In that case, don't do it again.
  if (added_analytics_js_ || found_snippet_) {
    return;
  }

  // No snippets have been found, and we haven't added any snippets
  // yet, so add one now.

  // Domain for this html page.
  GoogleString domain = driver()->google_url().Host().as_string();
  // HTTP vs. HTTPS - these are usually determined on the fly by js
  // in the ga snippet, but it's faster to determine it here.
  const char* kUrlPrefix = driver()->google_url().SchemeIs("https") ?
      "https://ssl" : "http://www";
  GoogleString js_text = StringPrintf(kGAJsSnippet, ga_id_.c_str(),
                                      domain.c_str(), kUrlPrefix);
  AddScriptNode(body, js_text, false);
  added_analytics_js_ = true;
  inserted_ga_snippets_count_->Add(1);
  return;
}

// Handle the end of a script tag.
// Look for a GA snippet in the script and record the findings so that we can
// optionally add the analytics js at the end of the body if no GA snippet is
// present on the page.
void InsertGAFilter::HandleEndScript(HtmlElement* script) {
  // There shouldn't be any "nested" script elements, but just
  // in case, don't reset things if the elements don't match.
  // Don't bother to look in here if we already found a snippet.
  // The buffer should also be empty in that case.
  if (script == script_element_ && !found_snippet_) {
    if (FoundSnippetInBuffer()) {
      // TODO(anupama): Handle the case where an analytics snippet is
      // present on the page, by storing this information in pcache.
      found_snippet_ = true;
    }
    script_element_ = NULL;
    buffer_.clear();
  }
}

void InsertGAFilter::EndElementImpl(HtmlElement* element) {
  switch (element->keyword()) {
    case HtmlName::kScript:
      HandleEndScript(element);
      break;
    case HtmlName::kBody:
      HandleEndBody(element);
      break;
    default:
      break;
  }
}

void InsertGAFilter::Characters(HtmlCharactersNode* characters) {
  if (script_element_ != NULL && !found_snippet_) {
    buffer_ += characters->contents();
  }
}

}  // namespace net_instaweb
