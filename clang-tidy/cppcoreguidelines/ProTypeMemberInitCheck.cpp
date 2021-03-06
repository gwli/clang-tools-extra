//===--- ProTypeMemberInitCheck.cpp - clang-tidy---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ProTypeMemberInitCheck.h"
#include "../utils/LexerUtils.h"
#include "../utils/Matchers.h"
#include "../utils/TypeTraits.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang::ast_matchers;
using namespace clang::tidy::matchers;
using llvm::SmallPtrSet;
using llvm::SmallPtrSetImpl;

namespace clang {
namespace tidy {
namespace cppcoreguidelines {

namespace {

// Iterate over all the fields in a record type, both direct and indirect (e.g.
// if the record contains an anonmyous struct). If OneFieldPerUnion is true and
// the record type (or indirect field) is a union, forEachField will stop after
// the first field.
template <typename T, typename Func>
void forEachField(const RecordDecl *Record, const T &Fields,
                  bool OneFieldPerUnion, Func &&Fn) {
  for (const FieldDecl *F : Fields) {
    if (F->isAnonymousStructOrUnion()) {
      if (const CXXRecordDecl *R = F->getType()->getAsCXXRecordDecl())
        forEachField(R, R->fields(), OneFieldPerUnion, Fn);
    } else {
      Fn(F);
    }

    if (OneFieldPerUnion && Record->isUnion())
      break;
  }
}

void removeFieldsInitializedInBody(
    const Stmt &Stmt, ASTContext &Context,
    SmallPtrSetImpl<const FieldDecl *> &FieldDecls) {
  auto Matches =
      match(findAll(binaryOperator(
                hasOperatorName("="),
                hasLHS(memberExpr(member(fieldDecl().bind("fieldDecl")))))),
            Stmt, Context);
  for (const auto &Match : Matches)
    FieldDecls.erase(Match.getNodeAs<FieldDecl>("fieldDecl"));
}

StringRef getName(const FieldDecl *Field) { return Field->getName(); }

StringRef getName(const RecordDecl *Record) {
  // Get the typedef name if this is a C-style anonymous struct and typedef.
  if (const TypedefNameDecl *Typedef = Record->getTypedefNameForAnonDecl())
    return Typedef->getName();
  return Record->getName();
}

// Creates comma separated list of decls requiring initialization in order of
// declaration.
template <typename R, typename T>
std::string
toCommaSeparatedString(const R &OrderedDecls,
                       const SmallPtrSetImpl<const T *> &DeclsToInit) {
  SmallVector<StringRef, 16> Names;
  for (const T *Decl : OrderedDecls) {
    if (DeclsToInit.count(Decl))
      Names.emplace_back(getName(Decl));
  }
  return llvm::join(Names.begin(), Names.end(), ", ");
}

SourceLocation getLocationForEndOfToken(const ASTContext &Context,
                                        SourceLocation Location) {
  return Lexer::getLocForEndOfToken(Location, 0, Context.getSourceManager(),
                                    Context.getLangOpts());
}

// There are 3 kinds of insertion placements:
enum class InitializerPlacement {
  // 1. The fields are inserted after an existing CXXCtorInitializer stored in
  // Where. This will be the case whenever there is a written initializer before
  // the fields available.
  After,

  // 2. The fields are inserted before the first existing initializer stored in
  // Where.
  Before,

  // 3. There are no written initializers and the fields will be inserted before
  // the constructor's body creating a new initializer list including the ':'.
  New
};

// An InitializerInsertion contains a list of fields and/or base classes to
// insert into the initializer list of a constructor. We use this to ensure
// proper absolute ordering according to the class declaration relative to the
// (perhaps improper) ordering in the existing initializer list, if any.
struct IntializerInsertion {
  IntializerInsertion(InitializerPlacement Placement,
                      const CXXCtorInitializer *Where)
      : Placement(Placement), Where(Where) {}

  SourceLocation getLocation(const ASTContext &Context,
                             const CXXConstructorDecl &Constructor) const {
    assert((Where != nullptr || Placement == InitializerPlacement::New) &&
           "Location should be relative to an existing initializer or this "
           "insertion represents a new initializer list.");
    SourceLocation Location;
    switch (Placement) {
    case InitializerPlacement::New:
      Location = utils::lexer::getPreviousNonCommentToken(
                     Context, Constructor.getBody()->getLocStart())
                     .getLocation();
      break;
    case InitializerPlacement::Before:
      Location = utils::lexer::getPreviousNonCommentToken(
                     Context, Where->getSourceRange().getBegin())
                     .getLocation();
      break;
    case InitializerPlacement::After:
      Location = Where->getRParenLoc();
      break;
    }
    return getLocationForEndOfToken(Context, Location);
  }

  std::string codeToInsert() const {
    assert(!Initializers.empty() && "No initializers to insert");
    std::string Code;
    llvm::raw_string_ostream Stream(Code);
    std::string joined =
        llvm::join(Initializers.begin(), Initializers.end(), "(), ");
    switch (Placement) {
    case InitializerPlacement::New:
      Stream << " : " << joined << "()";
      break;
    case InitializerPlacement::Before:
      Stream << " " << joined << "(),";
      break;
    case InitializerPlacement::After:
      Stream << ", " << joined << "()";
      break;
    }
    return Stream.str();
  }

  InitializerPlacement Placement;
  const CXXCtorInitializer *Where;
  SmallVector<std::string, 4> Initializers;
};

// Convenience utility to get a RecordDecl from a QualType.
const RecordDecl *getCanonicalRecordDecl(const QualType &Type) {
  if (const auto *RT = Type.getCanonicalType()->getAs<RecordType>())
    return RT->getDecl();
  return nullptr;
}

template <typename R, typename T>
SmallVector<IntializerInsertion, 16>
computeInsertions(const CXXConstructorDecl::init_const_range &Inits,
                  const R &OrderedDecls,
                  const SmallPtrSetImpl<const T *> &DeclsToInit) {
  SmallVector<IntializerInsertion, 16> Insertions;
  Insertions.emplace_back(InitializerPlacement::New, nullptr);

  typename R::const_iterator Decl = std::begin(OrderedDecls);
  for (const CXXCtorInitializer *Init : Inits) {
    if (Init->isWritten()) {
      if (Insertions.size() == 1)
        Insertions.emplace_back(InitializerPlacement::Before, Init);

      // Gets either the field or base class being initialized by the provided
      // initializer.
      const auto *InitDecl =
          Init->isAnyMemberInitializer()
              ? static_cast<const NamedDecl *>(Init->getAnyMember())
              : Init->getBaseClass()->getAsCXXRecordDecl();

      // Add all fields between current field up until the next intializer.
      for (; Decl != std::end(OrderedDecls) && *Decl != InitDecl; ++Decl) {
        if (const T *D = dyn_cast<T>(*Decl)) {
          if (DeclsToInit.count(D) > 0)
            Insertions.back().Initializers.emplace_back(getName(D));
        }
      }

      Insertions.emplace_back(InitializerPlacement::After, Init);
    }
  }

  // Add remaining decls that require initialization.
  for (; Decl != std::end(OrderedDecls); ++Decl) {
    if (const T *D = dyn_cast<T>(*Decl)) {
      if (DeclsToInit.count(D) > 0)
        Insertions.back().Initializers.emplace_back(getName(D));
    }
  }
  return Insertions;
}

// Gets the list of bases and members that could possibly be initialized, in
// order as they appear in the class declaration.
void getInitializationsInOrder(const CXXRecordDecl *ClassDecl,
                               SmallVectorImpl<const NamedDecl *> &Decls) {
  Decls.clear();
  for (const auto &Base : ClassDecl->bases()) {
    // Decl may be null if the base class is a template parameter.
    if (const NamedDecl *Decl = getCanonicalRecordDecl(Base.getType())) {
      Decls.emplace_back(Decl);
    }
  }
  forEachField(ClassDecl, ClassDecl->fields(), false,
               [&](const FieldDecl *F) { Decls.push_back(F); });
}

template <typename T>
void fixInitializerList(const ASTContext &Context, DiagnosticBuilder &Diag,
                        const CXXConstructorDecl *Ctor,
                        const SmallPtrSetImpl<const T *> &DeclsToInit) {
  // Do not propose fixes in macros since we cannot place them correctly.
  if (Ctor->getLocStart().isMacroID())
    return;

  SmallVector<const NamedDecl *, 16> OrderedDecls;
  getInitializationsInOrder(Ctor->getParent(), OrderedDecls);

  for (const auto &Insertion :
       computeInsertions(Ctor->inits(), OrderedDecls, DeclsToInit)) {
    if (!Insertion.Initializers.empty())
      Diag << FixItHint::CreateInsertion(Insertion.getLocation(Context, *Ctor),
                                         Insertion.codeToInsert());
  }
}

} // anonymous namespace

ProTypeMemberInitCheck::ProTypeMemberInitCheck(StringRef Name,
                                               ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      IgnoreArrays(Options.get("IgnoreArrays", false)) {}

void ProTypeMemberInitCheck::registerMatchers(MatchFinder *Finder) {
  if (!getLangOpts().CPlusPlus)
    return;

  auto IsUserProvidedNonDelegatingConstructor =
      allOf(isUserProvided(),
            unless(anyOf(isInstantiated(), isDelegatingConstructor())));
  auto IsNonTrivialDefaultConstructor = allOf(
      isDefaultConstructor(), unless(isUserProvided()),
      hasParent(cxxRecordDecl(unless(isTriviallyDefaultConstructible()))));
  Finder->addMatcher(
      cxxConstructorDecl(isDefinition(),
                         anyOf(IsUserProvidedNonDelegatingConstructor,
                               IsNonTrivialDefaultConstructor))
          .bind("ctor"),
      this);
  auto HasDefaultConstructor = hasInitializer(
      cxxConstructExpr(unless(requiresZeroInitialization()),
                       hasDeclaration(cxxConstructorDecl(
                           isDefaultConstructor(), unless(isUserProvided())))));
  Finder->addMatcher(
      varDecl(isDefinition(), HasDefaultConstructor,
              hasAutomaticStorageDuration(),
              hasType(recordDecl(has(fieldDecl()),
                                 isTriviallyDefaultConstructible())))
          .bind("var"),
      this);
}

void ProTypeMemberInitCheck::check(const MatchFinder::MatchResult &Result) {
  if (const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructorDecl>("ctor")) {
    // Skip declarations delayed by late template parsing without a body.
    if (!Ctor->getBody())
      return;
    checkMissingMemberInitializer(*Result.Context, Ctor);
    checkMissingBaseClassInitializer(*Result.Context, Ctor);
  } else if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("var")) {
    checkUninitializedTrivialType(*Result.Context, Var);
  }
}

void ProTypeMemberInitCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "IgnoreArrays", IgnoreArrays);
}

void ProTypeMemberInitCheck::checkMissingMemberInitializer(
    ASTContext &Context, const CXXConstructorDecl *Ctor) {
  const CXXRecordDecl *ClassDecl = Ctor->getParent();
  bool IsUnion = ClassDecl->isUnion();

  if (IsUnion && ClassDecl->hasInClassInitializer())
    return;

  // Gather all fields (direct and indirect) that need to be initialized.
  SmallPtrSet<const FieldDecl *, 16> FieldsToInit;
  forEachField(ClassDecl, ClassDecl->fields(), false, [&](const FieldDecl *F) {
    if (!F->hasInClassInitializer() &&
        utils::type_traits::isTriviallyDefaultConstructible(F->getType(),
                                                            Context))
      FieldsToInit.insert(F);
  });
  if (FieldsToInit.empty())
    return;

  for (const CXXCtorInitializer *Init : Ctor->inits()) {
    // Remove any fields that were explicitly written in the initializer list
    // or in-class.
    if (Init->isAnyMemberInitializer() && Init->isWritten()) {
      if (IsUnion)
        return; // We can only initialize one member of a union.
      FieldsToInit.erase(Init->getAnyMember());
    }
  }
  removeFieldsInitializedInBody(*Ctor->getBody(), Context, FieldsToInit);

  // Collect all fields in order, both direct fields and indirect fields from
  // anonmyous record types.
  SmallVector<const FieldDecl *, 16> OrderedFields;
  forEachField(ClassDecl, ClassDecl->fields(), false,
               [&](const FieldDecl *F) { OrderedFields.push_back(F); });

  // Collect all the fields we need to initialize, including indirect fields.
  SmallPtrSet<const FieldDecl *, 16> AllFieldsToInit;
  forEachField(ClassDecl, FieldsToInit, false,
               [&](const FieldDecl *F) { AllFieldsToInit.insert(F); });
  if (AllFieldsToInit.empty())
    return;

  DiagnosticBuilder Diag =
      diag(Ctor->getLocStart(),
           IsUnion
               ? "union constructor should initialize one of these fields: %0"
               : "constructor does not initialize these fields: %0")
      << toCommaSeparatedString(OrderedFields, AllFieldsToInit);

  // Do not propose fixes in macros since we cannot place them correctly.
  if (Ctor->getLocStart().isMacroID())
    return;

  // Collect all fields but only suggest a fix for the first member of unions,
  // as initializing more than one union member is an error.
  SmallPtrSet<const FieldDecl *, 16> FieldsToFix;
  forEachField(ClassDecl, FieldsToInit, true, [&](const FieldDecl *F) {
    // Don't suggest fixes for enums because we don't know a good default.
    if (!F->getType()->isEnumeralType())
      FieldsToFix.insert(F);
  });
  if (FieldsToFix.empty())
    return;

  // Use in-class initialization if possible.
  if (Context.getLangOpts().CPlusPlus11) {
    for (const FieldDecl *Field : FieldsToFix) {
      Diag << FixItHint::CreateInsertion(
          getLocationForEndOfToken(Context, Field->getSourceRange().getEnd()),
          "{}");
    }
  } else {
    // Otherwise, rewrite the constructor's initializer list.
    fixInitializerList(Context, Diag, Ctor, FieldsToFix);
  }
}

void ProTypeMemberInitCheck::checkMissingBaseClassInitializer(
    const ASTContext &Context, const CXXConstructorDecl *Ctor) {
  const CXXRecordDecl *ClassDecl = Ctor->getParent();

  // Gather any base classes that need to be initialized.
  SmallVector<const RecordDecl *, 4> AllBases;
  SmallPtrSet<const RecordDecl *, 4> BasesToInit;
  for (const CXXBaseSpecifier &Base : ClassDecl->bases()) {
    if (const auto *BaseClassDecl = getCanonicalRecordDecl(Base.getType())) {
      AllBases.emplace_back(BaseClassDecl);
      if (!BaseClassDecl->field_empty() &&
          utils::type_traits::isTriviallyDefaultConstructible(Base.getType(),
                                                              Context))
        BasesToInit.insert(BaseClassDecl);
    }
  }

  if (BasesToInit.empty())
    return;

  // Remove any bases that were explicitly written in the initializer list.
  for (const CXXCtorInitializer *Init : Ctor->inits()) {
    if (Init->isBaseInitializer() && Init->isWritten())
      BasesToInit.erase(Init->getBaseClass()->getAsCXXRecordDecl());
  }

  if (BasesToInit.empty())
    return;

  DiagnosticBuilder Diag =
      diag(Ctor->getLocStart(),
           "constructor does not initialize these bases: %0")
      << toCommaSeparatedString(AllBases, BasesToInit);

  fixInitializerList(Context, Diag, Ctor, BasesToInit);
}

void ProTypeMemberInitCheck::checkUninitializedTrivialType(
    const ASTContext &Context, const VarDecl *Var) {
  DiagnosticBuilder Diag =
      diag(Var->getLocStart(), "uninitialized record type: %0") << Var;

  Diag << FixItHint::CreateInsertion(
      getLocationForEndOfToken(Context, Var->getSourceRange().getEnd()),
      Context.getLangOpts().CPlusPlus11 ? "{}" : " = {}");
}

} // namespace cppcoreguidelines
} // namespace tidy
} // namespace clang
