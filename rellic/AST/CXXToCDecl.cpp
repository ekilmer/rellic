/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rellic/AST/CXXToCDecl.h"

#include <clang/AST/Mangle.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/Compat/Mangle.h"
#include "rellic/AST/Compat/Type.h"
#include "rellic/AST/Util.h"

namespace rellic {

namespace {

static std::string GetMangledName(clang::NamedDecl *decl) {
  auto mangler = decl->getASTContext().createMangleContext();
  std::string buffer;
  if (mangler->shouldMangleDeclName(decl)) {
    llvm::raw_string_ostream os(buffer);
    if (auto type_decl = clang::dyn_cast<clang::TypeDecl>(decl)) {
      auto type = clang::QualType(type_decl->getTypeForDecl(), 0);
      mangler->mangleTypeName(type, os);
    } else if (auto cst = clang::dyn_cast<clang::CXXConstructorDecl>(decl)) {
      MangleNameCXXCtor(mangler, cst, os);
      // mangler->mangleCXXCtor(cst, clang::Ctor_Complete, os);
    } else if (auto dst = clang::dyn_cast<clang::CXXDestructorDecl>(decl)) {
      MangleNameCXXDtor(mangler, dst, os);
      // mangler->mangleCXXDtor(dst, clang::Dtor_Complete, os);
    } else {
      mangler->mangleName(decl, os);
    }
    os.flush();
  }
  return buffer.empty() ? decl->getNameAsString() : buffer;
}

}  // namespace

CXXToCDeclVisitor::CXXToCDeclVisitor(clang::ASTContext &ctx)
    : ast_ctx(ctx), c_tu(ctx.getTranslationUnitDecl()) {}

clang::QualType CXXToCDeclVisitor::GetAsCType(clang::QualType type) {
  const clang::Type *result;
  if (auto ptr = type->getAs<clang::PointerType>()) {
    // Get a C pointer equivalent
    auto pointee = GetAsCType(ptr->getPointeeType());
    result = ast_ctx.getPointerType(pointee).getTypePtr();
  } else if (auto ref = type->getAs<clang::ReferenceType>()) {
    // Get a C pointer equivalent
    auto pointee = GetAsCType(ref->getPointeeType());
    auto ptr = ast_ctx.getPointerType(pointee);
    // Add `_Nonnull` attribute
    auto attr_type = ast_ctx.getAttributedType(attr_nonnull, ptr, ptr);
    result = attr_type.getTypePtr();
  } else if (auto cls = type->getAsCXXRecordDecl()) {
    // Handle class-type attributes by translating to struct types
    auto iter = c_decls.find(cls);
    CHECK(iter != c_decls.end())
        << "C struct for class" << cls->getNameAsString() << " does not exist";
    auto decl = clang::cast<clang::RecordDecl>(iter->second);
    result = decl->getTypeForDecl();
  } else {
    result = type.getTypePtr();
  }
  return clang::QualType(result, type.getQualifiers().getAsOpaqueValue());
}

bool CXXToCDeclVisitor::VisitFunctionDecl(clang::FunctionDecl *cxx_func) {
  auto name = cxx_func->getNameAsString();
  DLOG(INFO) << "VisitFunctionDecl: " << name;
  // Check if the corresponding C function doesn't exist already
  if (c_decls.count(cxx_func)) {
    LOG(WARNING) << "Asking to re-generate function: " << name << "; returning";
    return true;
  }
  // Gather parameter types
  auto cxx_proto = clang::cast<clang::FunctionProtoType>(cxx_func->getType());
  std::vector<clang::QualType> param_types(cxx_proto->param_type_begin(),
                                           cxx_proto->param_type_end());
  // Create function prototype
  auto ret_type = GetAsCType(cxx_proto->getReturnType());
  auto func_type = ast_ctx.getFunctionType(ret_type, param_types,
                                           cxx_proto->getExtProtoInfo());
  // Declare the C function
  auto func_id = CreateIdentifier(ast_ctx, GetMangledName(cxx_func));
  auto func_decl = CreateFunctionDecl(ast_ctx, c_tu, func_id, func_type);
  // Declare parameters
  std::vector<clang::ParmVarDecl *> param_decls;
  for (auto cxx_param : cxx_func->parameters()) {
    auto param_id = CreateIdentifier(ast_ctx, cxx_param->getNameAsString());
    auto param_type = GetAsCType(cxx_param->getType());
    param_decls.push_back(
        CreateParmVarDecl(ast_ctx, func_decl, param_id, param_type));
  }
  // Set C function parameters
  func_decl->setParams(param_decls);
  // Save to C translation unit
  if (!clang::isa<clang::CXXMethodDecl>(cxx_func)) {
    // c_tu->addDecl(func_decl);
  } else {
    c_decls[cxx_func] = func_decl;
  }
  // Done
  return true;
}

bool CXXToCDeclVisitor::VisitCXXMethodDecl(clang::CXXMethodDecl *method) {
  auto name = method->getNameAsString();
  DLOG(INFO) << "VisitCXXMethodDecl: " << name;
  // Get the result of `VisitFunctionDecl`
  auto func_iter = c_decls.find(method);
  CHECK(func_iter != c_decls.end())
      << "Method " << name
      << " does not have a C function equivalent created by VisitFunctionDecl";
  // Get C struct equivalent of `method` parent class
  auto struct_iter = c_decls.find(method->getParent());
  CHECK(struct_iter != c_decls.end())
      << "Method " << name << " does not have a parent";
  // Get the `this` pointer type
  auto struct_decl = clang::cast<clang::RecordDecl>(struct_iter->second);
  auto struct_type = struct_decl->getTypeForDecl();
  auto this_type = ast_ctx.getPointerType(clang::QualType(struct_type, 0));
  // Transfer stuff from the declaration made by `VisitFunctionDecl`
  auto old_func = clang::cast<clang::FunctionDecl>(func_iter->second);
  auto old_proto = clang::cast<clang::FunctionProtoType>(old_func->getType());
  std::vector<clang::QualType> param_types({this_type});
  param_types.insert(param_types.end(), old_proto->param_type_begin(),
                     old_proto->param_type_end());
  // Create function prototype
  auto func_type = ast_ctx.getFunctionType(
      old_proto->getReturnType(), param_types, old_proto->getExtProtoInfo());
  // Declare the C function
  auto func_decl =
      CreateFunctionDecl(ast_ctx, c_tu, old_func->getIdentifier(), func_type);
  // Declare parameters
  auto this_id = CreateIdentifier(ast_ctx, "this");
  auto this_decl = CreateParmVarDecl(ast_ctx, func_decl, this_id, this_type);
  std::vector<clang::ParmVarDecl *> param_decls({this_decl});
  param_decls.insert(param_decls.end(), old_func->param_begin(),
                     old_func->param_end());
  // Set C function parameters
  func_decl->setParams(param_decls);
  // Save to C translation unit
  c_tu->addDecl(func_decl);
  // Done
  return true;
}

bool CXXToCDeclVisitor::VisitRecordDecl(clang::RecordDecl *record) {
  // auto name = record->getNameAsString();
  // DLOG(INFO) << "VisitRecordDecl: " << name;
  return true;
}

bool CXXToCDeclVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *cls) {
  auto name = cls->getNameAsString();
  DLOG(INFO) << "VisitCXXRecordDecl: " << name;
  if (c_decls.count(cls)) {
    LOG(WARNING) << "Asking to re-generate class " << name << " ; returning";
    return true;
  }
  // Create a vtable
  if (cls->isPolymorphic()) {
  }
  // Create the C struct definition
  auto id = CreateIdentifier(ast_ctx, GetMangledName(cls));
  auto decl = CreateStructDecl(ast_ctx, c_tu, id);
  // Complete the C struct definition
  clang::cast<clang::RecordDecl>(decl)->completeDefinition();
  // Save the result
  c_decls[cls] = decl;
  // Add the C struct to the C translation unit
  c_tu->addDecl(decl);
  // Done
  return true;
}

bool CXXToCDeclVisitor::VisitFieldDecl(clang::FieldDecl *field) {
  auto name = field->getNameAsString();
  DLOG(INFO) << "FieldDecl: " << name;
  // Get parent C struct
  auto iter = c_decls.find(field->getParent());
  CHECK(iter != c_decls.end()) << "Field " << name << " does not have a parent";
  auto parent = clang::cast<clang::RecordDecl>(iter->second);
  // Create the field
  auto id = CreateIdentifier(ast_ctx, field->getName().str());
  auto type = GetAsCType(field->getType());
  auto decl = CreateFieldDecl(ast_ctx, parent, id, type);
  // Save the result
  c_decls[field] = decl;
  // Add the field to it's parent struct
  parent->addDecl(decl);
  // Done
  return true;
}

}  // namespace rellic