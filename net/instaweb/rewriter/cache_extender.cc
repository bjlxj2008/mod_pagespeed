/*
 * Copyright 2010 Google Inc.
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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/cache_extender.h"

#include <memory>

#include "base/logging.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/javascript_code_block.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "pagespeed/opt/logging/enums.pb.h"

namespace net_instaweb {
class MessageHandler;
class RewriteContext;

// names for Statistics variables.
const char CacheExtender::kCacheExtensions[] = "cache_extensions";
const char CacheExtender::kNotCacheable[] = "not_cacheable";

// We do not want to bother to extend the cache lifetime for any resource
// that is already cached for a month.
const int64 kMinThresholdMs = Timer::kMonthMs;

class CacheExtender::Context : public SingleRewriteContext {
 public:
  Context(CacheExtender* extender, RewriteDriver* driver,
          RewriteContext* parent)
      : SingleRewriteContext(driver, parent,
                             NULL /* no resource context */),
        extender_(extender) {}
  virtual ~Context() {}

  virtual void Render();
  virtual void RewriteSingle(const ResourcePtr& input,
                             const OutputResourcePtr& output);
  virtual const char* id() const { return extender_->id(); }
  virtual OutputResourceKind kind() const { return kOnTheFlyResource; }

 private:
  CacheExtender* extender_;
  DISALLOW_COPY_AND_ASSIGN(Context);
};

CacheExtender::CacheExtender(RewriteDriver* driver)
    : RewriteFilter(driver) {
  Statistics* stats = server_context()->statistics();
  extension_count_ = stats->GetVariable(kCacheExtensions);
  not_cacheable_count_ = stats->GetVariable(kNotCacheable);
}

CacheExtender::~CacheExtender() {}

void CacheExtender::InitStats(Statistics* statistics) {
  statistics->AddVariable(kCacheExtensions);
  statistics->AddVariable(kNotCacheable);
}

bool CacheExtender::ShouldRewriteResource(
    const ResponseHeaders* headers, int64 now_ms,
    const ResourcePtr& input_resource, const StringPiece& url,
    CachedResult* result) const {
  const ContentType* input_resource_type = input_resource->type();
  if (input_resource_type == NULL) {
    return false;
  }
  if (input_resource_type->type() == ContentType::kJavascript &&
      driver()->options()->avoid_renaming_introspective_javascript() &&
      JavascriptCodeBlock::UnsafeToRename(input_resource->contents())) {
    CHECK(result != NULL);
    result->add_debug_message(JavascriptCodeBlock::kIntrospectionComment);
    return false;
  }
  if ((headers->CacheExpirationTimeMs() - now_ms) < kMinThresholdMs) {
    // This also includes the case where a previous filter rewrote this.
    return true;
  }
  UrlNamer* url_namer = driver()->server_context()->url_namer();
  GoogleUrl origin_gurl(url);

  // We won't initiate a CacheExtender::Context with a pagespeed
  // resource URL.  However, an upstream filter might have rewritten
  // the resource after we queued the request, but before our
  // context is asked to rewrite it.  So we have to check again now
  // that the resource URL is finalized.
  if (server_context()->IsPagespeedResource(origin_gurl)) {
    return false;
  }

  if (url_namer->ProxyMode()) {
    return !url_namer->IsProxyEncoded(origin_gurl);
  }
  const DomainLawyer* lawyer = driver()->options()->domain_lawyer();

  // We return true for IsProxyMapped because when reconstructing
  // MAPPED_DOMAIN/file.pagespeed.ce.HASH.ext we won't be changing
  // the domain (WillDomainChange==false) but we want this function
  // to return true so that we can reconstruct the cache-extension and
  // serve the result with long public caching.  Without IsProxyMapped,
  // we'd serve the result with cache-control:private,max-age=300.
  return (lawyer->IsProxyMapped(origin_gurl) ||
          lawyer->WillDomainChange(origin_gurl));
}

void CacheExtender::StartElementImpl(HtmlElement* element) {
  resource_tag_scanner::UrlCategoryVector attributes;
  resource_tag_scanner::ScanElement(element, driver()->options(), &attributes);
  for (int i = 0, n = attributes.size(); i < n; ++i) {
    bool may_load = false;
    switch (attributes[i].category) {
      case semantic_type::kStylesheet:
        may_load = driver()->MayCacheExtendCss();
        break;
      case semantic_type::kImage:
        may_load = driver()->MayCacheExtendImages();
        break;
      case semantic_type::kScript:
        may_load = driver()->MayCacheExtendScripts();
        break;
      default:
        // Does the url in the attribute end in .pdf, ignoring query params?
        if (attributes[i].url->DecodedValueOrNull() != NULL
            && driver()->MayCacheExtendPdfs()) {
        GoogleUrl url(driver()->base_url(),
                      attributes[i].url->DecodedValueOrNull());
        if (url.IsWebValid() && StringCaseEndsWith(
                url.LeafSansQuery(), kContentTypePdf.file_extension())) {
          may_load = true;
        }
      }
      break;
    }
    if (!may_load) {
      continue;
    }

    // TODO(jmarantz): We ought to be able to domain-shard even if the
    // resources are non-cacheable or privately cacheable.
    if (driver()->IsRewritable(element)) {
      ResourcePtr input_resource(CreateInputResourceOrInsertDebugComment(
          attributes[i].url->DecodedValueOrNull(), element));
      if (input_resource.get() == NULL) {
        continue;
      }

      GoogleUrl input_gurl(input_resource->url());
      if (server_context()->IsPagespeedResource(input_gurl)) {
        continue;
      }

      ResourceSlotPtr slot(driver()->GetSlot(
          input_resource, element, attributes[i].url));
      Context* context = new Context(this, driver(), NULL /* not nested */);
      context->AddSlot(slot);
      driver()->InitiateRewrite(context);
    }
  }
}

bool CacheExtender::ComputeOnTheFly() const {
  return true;
}

void CacheExtender::Context::RewriteSingle(
    const ResourcePtr& input_resource,
    const OutputResourcePtr& output_resource) {
  RewriteDone(
      extender_->RewriteLoadedResource(
          input_resource, output_resource, output_partition(0)), 0);
}

void CacheExtender::Context::Render() {
  if (num_output_partitions() == 1 && output_partition(0)->optimizable()) {
    extender_->extension_count_->Add(1);
    // Log applied rewriter id. Here, we care only about non-nested
    // cache extensions, and that too, those occurring in synchronous
    // flows only.
    if (Driver() != NULL) {
      ResourceSlotPtr the_slot = slot(0);
      if (the_slot->resource().get() != NULL &&
          the_slot->resource()->type() != NULL) {
        const char* filter_id = id();
        const ContentType* type = the_slot->resource()->type();
        if (type->type() == ContentType::kCss) {
          filter_id = RewriteOptions::FilterId(
              RewriteOptions::kExtendCacheCss);
        } else if (type->type() == ContentType::kJavascript) {
          filter_id = RewriteOptions::FilterId(
              RewriteOptions::kExtendCacheScripts);
        } else if (type->IsImage()) {
          filter_id = RewriteOptions::FilterId(
              RewriteOptions::kExtendCacheImages);
        }
        // TODO(anupama): Log cache extension for pdfs etc.
        Driver()->log_record()->SetRewriterLoggingStatus(
            filter_id,
            the_slot->resource()->url(),
            RewriterApplication::APPLIED_OK);
      }
    }
  }
}

RewriteResult CacheExtender::RewriteLoadedResource(
    const ResourcePtr& input_resource,
    const OutputResourcePtr& output_resource,
    // TODO(jmaessen): does this belong in CacheExtender::Context? to this
    // method and ShouldRewriteResource.
    CachedResult* result) {
  CHECK(input_resource->loaded());

  MessageHandler* message_handler = driver()->message_handler();
  const ResponseHeaders* headers = input_resource->response_headers();
  GoogleString url = input_resource->url();
  int64 now_ms = server_context()->timer()->NowMs();

  // See if the resource is cacheable; and if so whether there is any need
  // to cache extend it.
  bool ok = false;
  const ContentType* output_type = NULL;
  if (!server_context()->http_cache()->force_caching() &&
      !headers->IsProxyCacheable()) {
    // Note: RewriteContextTest.PreserveNoCacheWithFailedRewrites
    // relies on CacheExtender failing rewrites in this case.
    // If you change this behavior that test MUST be updated as it covers
    // security.
    not_cacheable_count_->Add(1);
  } else if (ShouldRewriteResource(
                 headers, now_ms, input_resource,url, result)) {
    // We must be careful what Content-Types we allow to be cache extended.
    // Specifically, we do not want to cache extend any Content-Types that
    // could execute scripts when loaded in a browser because that could
    // open XSS vectors in case of system misconfiguration.
    //
    // We whitelist a set of safe Content-Types here.
    //
    // TODO(sligocki): Should we whitelist more Content-Types as well?
    // We would also have to find and rewrite the URLs to these resources
    // if we want to cache extend them.
    const ContentType* input_type = input_resource->type();
    if (input_type->IsImage() ||  // images get sniffed only to other images
        (input_type->type() == ContentType::kPdf &&
         driver()->MayCacheExtendPdfs()) ||  // Don't accept PDFs by default.
        input_type->type() == ContentType::kCss ||  // CSS + JS left as-is.
        input_type->type() == ContentType::kJavascript) {
      output_type = input_type;
      ok = true;
    } else {
      // Fail to cache extend a file that isn't an approved type.
      ok = false;

      // If we decide not to fail to cache extend unapproved types, we
      // should convert their Content-Type to text/plain because as per
      // http://mimesniff.spec.whatwg.org/ it will never get turned into
      // anything dangerous.
      output_type = &kContentTypeText;
    }
  }

  if (!ok) {
    return kRewriteFailed;
  }

  StringPiece contents(input_resource->contents());
  GoogleString transformed_contents;
  StringWriter writer(&transformed_contents);
  GoogleUrl input_resource_gurl(input_resource->url());
  if (output_type->type() == ContentType::kCss) {
    switch (driver()->ResolveCssUrls(input_resource_gurl,
                                     output_resource->resolved_base(),
                                     contents, &writer, message_handler)) {
      case RewriteDriver::kNoResolutionNeeded:
        break;
      case RewriteDriver::kWriteFailed:
        return kRewriteFailed;
      case RewriteDriver::kSuccess:
        // TODO(jmarantz): find a mechanism to write this directly into
        // the HTTPValue so we can reduce the number of times that we
        // copy entire resources.
        contents = transformed_contents;
        break;
    }
  }

  server_context()->MergeNonCachingResponseHeaders(
      input_resource, output_resource);
  if (driver()->Write(ResourceVector(1, input_resource),
                      contents,
                      output_type,
                      input_resource->charset(),
                      output_resource.get())) {
    return kRewriteOk;
  } else {
    return kRewriteFailed;
  }
}

RewriteContext* CacheExtender::MakeRewriteContext() {
  return new Context(this, driver(), NULL /*not nested*/);
}

RewriteContext* CacheExtender::MakeNestedContext(
    RewriteContext* parent, const ResourceSlotPtr& slot) {
  Context* context = new Context(this, NULL /* driver*/, parent);
  context->AddSlot(slot);
  return context;
}

}  // namespace net_instaweb
